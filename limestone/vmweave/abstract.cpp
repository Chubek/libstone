#include "internal.hpp"
extern "C" {
#include "AbstractAST.h"
int vw_parse_operations(const char*, vw_ast_arena*, vw_ast_operation_seq*, size_t*);
}
#include <charconv>
#include <mutex>

namespace limestone::vmweave {
namespace {
enum class Argument { None, Constant, Operand, Field };
struct Operation {
  AbstractOpcode opcode;
  std::string_view name;
  Argument argument;
  int inputs, outputs;
  std::string_view flow = "next";
};
using enum AbstractOpcode;
constexpr Operation operations[] = {
  {Constant,"const",Argument::Constant,0,1},
  {Operand,"operand",Argument::Operand,0,1},
  {Load,"load",Argument::Field,0,1}, {Store,"store",Argument::Field,1,0},
  {Drop,"drop",Argument::None,1,0}, {Dup,"dup",Argument::None,1,2}, {Swap,"swap",Argument::None,2,2},
  {Add,"add",Argument::None,2,1}, {Sub,"sub",Argument::None,2,1},
  {Mul,"mul",Argument::None,2,1}, {Div,"div",Argument::None,2,1},
  {Eq,"eq",Argument::None,2,1}, {Lt,"lt",Argument::None,2,1},
  {Jump,"jump",Argument::Operand,0,0,"jump"},
  {JumpZero,"jump_zero",Argument::Operand,1,0,"branch"},
  {Halt,"halt",Argument::None,0,0,"halt"}
};
constexpr size_t max_bytes=64*1024, max_operations=256;
struct Arena {
  vw_ast_arena value;
  Arena() {vw_ast_arena_init(&value);}
  ~Arena() {vw_ast_arena_destroy(&value);}
};
std::string_view view(vw_ast_string_t text) {return {text.data,text.length};}
std::optional<uint64_t> decimal(std::string_view text) {
  uint64_t value;
  auto [at,ec]=std::from_chars(text.data(),text.data()+text.size(),value);
  if(ec!=std::errc{} || at!=text.data()+text.size()) return std::nullopt;
  return value;
}
std::string location(const Location& loc, std::string_view text, size_t offset) {
  size_t line=loc.line ? loc.line : 1, column=1;
  for(char c:text.substr(0,offset)) {
    if(c=='\n') {++line; column=1;}
    else ++column;
  }
  return (loc.file.empty()?"<abstract>":loc.file)+":"+std::to_string(line)+":"+std::to_string(column);
}
}

Result<AbstractProgram> parse_abstract(std::string_view text, const Location& loc) {
  auto error=[&](size_t pos, std::string message, Error::Code code=Error::Code::Parse) {
    return Result<AbstractProgram>::err({code,"VMWeave: "+location(loc,text,pos)+": abstract semantics: "+message});
  };
  if(text.size()>max_bytes) return error(0,"body exceeds 64 KiB",Error::Code::ResourceLimit);
  if(auto nul=text.find('\0'); nul!=std::string_view::npos) return error(nul,"NUL in body");
  Arena arena;
  vw_ast_operation_seq nodes{};
  {
    static std::mutex parse_lock;
    std::lock_guard lock(parse_lock);
    size_t offset=0;
    if(!vw_parse_operations(std::string(text).c_str(),&arena.value,&nodes,&offset))
      return error(offset,"expected a semicolon-terminated operation");
  }
  if(nodes.length>max_operations) return error(0,"body exceeds 256 operations",Error::Code::ResourceLimit);
  AbstractProgram program;
  program.reserve(nodes.length);
  for(size_t i=0;i<nodes.length;++i) {
    const auto& node=*nodes.items[i];
    std::string_view name, arg;
    switch(node.kind) {
      case vw_ast_Bare_kind: name=view(node.v.Bare.name); break;
      case vw_ast_Numbered_kind: name=view(node.v.Numbered.name); arg=view(node.v.Numbered.spelling); break;
      case vw_ast_Named_kind: name=view(node.v.Named.name); arg=view(node.v.Named.field); break;
      default: return error(node.offset,"invalid operation");
    }
    auto it=std::find_if(std::begin(operations),std::end(operations),[&](const auto& o){return o.name==name;});
    if(it==std::end(operations)) return error(node.offset,"unknown operation '"+std::string(name)+"'");
    AbstractOperation op{it->opcode,{},node.offset};
    switch(it->argument) {
      case Argument::None:
        if(node.kind!=vw_ast_Bare_kind) return error(node.offset,"'"+std::string(name)+"' takes no argument");
        break;
      case Argument::Constant: {
        if(node.kind!=vw_ast_Numbered_kind) return error(node.offset,"const requires a decimal cell value");
        bool negative=arg.starts_with('-');
        auto value=decimal(negative?arg.substr(1):arg);
        if(!value) return error(node.offset,"constant is out of range");
        op.argument=AbstractConstant{*value,negative && *value!=0};
        break;
      }
      case Argument::Operand: {
        auto index=node.kind==vw_ast_Numbered_kind?decimal(arg):std::nullopt;
        if(!index || *index>=16) return error(node.offset,"expected a zero-based operand index in 0..15");
        op.argument=static_cast<uint32_t>(*index);
        break;
      }
      case Argument::Field:
        if(node.kind!=vw_ast_Named_kind) return error(node.offset,"expected a scalar state field name");
        op.argument=std::string(arg); break;
    }
    program.push_back(std::move(op));
  }
  return Result<AbstractProgram>::ok(std::move(program));
}

Result<AbstractProfile> profile_abstract(const Specification& s, const Instruction& instruction) {
  const auto& program=*instruction.abstract;
  auto error=[&](size_t i,std::string message) {
    return Result<AbstractProfile>::err({Error::Code::InvalidArgument,"abstract operation "+std::to_string(i+1)+": "+message});
  };
  if(program.size()>max_operations) return error(0,"body exceeds 256 operations");
  int needed=0, depth=0;
  size_t bytes=0;
  AbstractProfile profile;
  for(size_t i=0;i<program.size();++i) {
    const auto& op=program[i];
    auto it=std::find_if(std::begin(operations),std::end(operations),[&](const auto& o){return o.opcode==op.opcode;});
    if(it==std::end(operations)) return error(i,"unknown opcode");
    bytes+=it->name.size()+2; // semicolon and newline
    switch(it->argument) {
      case Argument::None:
        if(!std::holds_alternative<std::monostate>(op.argument)) return error(i,"unexpected argument");
        break;
      case Argument::Constant: {
        auto value=std::get_if<AbstractConstant>(&op.argument);
        if(!value) return error(i,"missing constant");
        // Keep intptr_t constants portable across both supported 32- and
        // 64-bit hosts. Use the fixed-width cell types for wider literals.
        uint64_t max=s.cell=="intptr_t"?INT32_MAX:INT64_MAX;
        if((value->negative && !value->magnitude) ||
           (s.cell=="uint64_t"?value->negative:value->magnitude>max+value->negative))
          return error(i,"constant is out of range for "+s.cell);
        bytes+=1+value->negative+std::to_string(value->magnitude).size();
        break;
      }
      case Argument::Operand: {
        auto index=std::get_if<uint32_t>(&op.argument);
        if(!index || *index>=instruction.operands.size()) return error(i,"undeclared operand index");
        auto kind=op.opcode==AbstractOpcode::Operand?"cell":"label";
        if(instruction.operands[*index]!=kind) return error(i,std::string(it->name)+" requires a "+kind+" operand");
        bytes+=*index<10?2:3;
        break;
      }
      case Argument::Field: {
        auto name=std::get_if<std::string>(&op.argument);
        if(!name) return error(i,"missing state field name");
        if(name->size()>max_bytes) return error(i,"canonical abstract body exceeds 64 KiB");
        bytes+=1+name->size();
        auto f=std::find_if(s.fields.begin(),s.fields.end(),[&](const auto& v){return v.name==*name;});
        if(f==s.fields.end()) return error(i,"unknown state field '"+*name+"'");
        if(f->count || (s.cell=="int64_t"?f->type!="i64":s.cell=="uint64_t"?f->type!="u64":true))
          return error(i,"state field '"+*name+"' must be scalar and match a fixed-width cell type");
        break;
      }
    }
    if(bytes>max_bytes) return error(i,"canonical abstract body exceeds 64 KiB");
    if(it->flow!="next" && i+1!=program.size()) return error(i,"control transfer must end the body");
    needed=std::max(needed,it->inputs-depth);
    depth+=it->outputs-it->inputs;
    profile.flow=it->flow;
  }
  profile.inputs=static_cast<size_t>(needed);
  profile.outputs=static_cast<size_t>(needed+depth);
  if(std::max(profile.inputs,profile.outputs)>s.stack_capacity) return error(0,"stack effect exceeds VM capacity");
  return Result<AbstractProfile>::ok(profile);
}

std::string print_abstract(const AbstractProgram& program) {
  std::ostringstream out;
  for(const auto& op:program) {
    auto it=std::find_if(std::begin(operations),std::end(operations),[&](const auto& o){return o.opcode==op.opcode;});
    out<<it->name;
    if(auto value=std::get_if<AbstractConstant>(&op.argument)) {
      out<<' '; if(value->negative) out<<'-'; out<<value->magnitude;
    } else if(auto index=std::get_if<uint32_t>(&op.argument)) out<<' '<<*index;
    else if(auto field=std::get_if<std::string>(&op.argument)) out<<' '<<*field;
    out<<";\n";
  }
  return out.str();
}
}
