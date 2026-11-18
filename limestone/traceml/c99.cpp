#include "c99.hpp"
#include "primitives.hpp"
#include <locale>
#include <sstream>

namespace limestone::traceml {
namespace {
bool valid_module(std::string_view name) {
  if(name.empty()||name.size()>96)return false;
  auto alpha=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z');};
  return (alpha(name.front())||name.front()=='_')&&std::all_of(name.begin(),name.end(),
    [&](char c){return alpha(c)||c=='_'||(c>='0'&&c<='9');});
}

std::string literal(int64_t value) {
  if(value==INT64_MIN)return "INT64_MIN";
  if(value<0)return "-INT64_C("+std::to_string(-value)+")";
  return "INT64_C("+std::to_string(value)+")";
}

const char* mnemonic(uint32_t op) {
  constexpr const char* names[] = {"TRACEML_CONST","TRACEML_LOCAL","TRACEML_LAMBDA","TRACEML_APPLY",
    "TRACEML_IF","TRACEML_BEGIN","TRACEML_ADD","TRACEML_SUB","TRACEML_MUL",
    "TRACEML_EQ","TRACEML_LT","TRACEML_NEG"};
  static_assert(std::size(names)==TRACEML_NEG+1);
  return names[op];
}

struct Module {
  static constexpr size_t max_records=1000000;
  std::vector<traceml_node_v1> nodes;
  std::vector<uint32_t> edges, forms;
  std::vector<std::string_view> bindings;

  uint32_t lower(const Expr& expr) {
    if(expr.offset>UINT32_MAX||nodes.size()>=max_records||expr.children.size()>max_records-edges.size())
      throw Error{Error::Code::ResourceLimit,"TraceML C99 module exceeds code or source-location limits"};
    traceml_node_v1 node{};
    node.offset=static_cast<uint32_t>(expr.offset);
    node.line=expr.line;
    node.column=expr.column;
    using enum Expr::Kind;
    switch(expr.kind) {
      case Integer: node.opcode=TRACEML_CONST; node.integer=expr.integer; break;
      case Symbol: {
        const auto found=std::find(bindings.rbegin(),bindings.rend(),expr.atom);
        if(found==bindings.rend()) {
          auto primitive=detail::primitive(expr.atom);
          if(!primitive)throw Error{Error::Code::Internal,"unresolved TraceML symbol after verification"};
          node.opcode=primitive->opcode;
        }else {
          node.opcode=TRACEML_LOCAL;
          node.lexical_index=static_cast<uint32_t>(found-bindings.rbegin());
        }
        break;
      }
      case Lambda: node.opcode=TRACEML_LAMBDA; break;
      case Apply: node.opcode=TRACEML_APPLY; break;
      case If: node.opcode=TRACEML_IF; break;
      case Begin: node.opcode=TRACEML_BEGIN; break;
    }
    std::vector<uint32_t> children;
    children.reserve(expr.children.size());
    if(expr.kind==Lambda)bindings.push_back(expr.atom);
    for(const auto& c:expr.children)children.push_back(lower(*c));
    if(expr.kind==Lambda)bindings.pop_back();
    if(nodes.size()>=max_records||children.size()>max_records-edges.size())
      throw Error{Error::Code::ResourceLimit,"TraceML C99 module exceeds code limits"};
    node.first_edge=static_cast<uint32_t>(edges.size());
    node.edge_count=static_cast<uint32_t>(children.size());
    edges.insert(edges.end(),children.begin(),children.end());
    nodes.push_back(node);
    return static_cast<uint32_t>(nodes.size()-1);
  }

  std::string render(std::string_view name) const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out<<"/* Generated TraceML C99 module. Link with libtraceml. */\n"
       <<"#include <traceml/runtime.h>\n\n"
       <<"static const traceml_node_v1 trace_nodes[] = {\n";
    for(const auto& n:nodes)
      out<<"  {"<<mnemonic(n.opcode)<<", "<<n.first_edge<<"u, "<<n.edge_count<<"u, "
         <<n.lexical_index<<"u, "<<literal(n.integer)<<", "<<n.offset<<"u, "
         <<n.line<<"u, "<<n.column<<"u},\n";
    out<<"};\n";
    auto indices=[&](const char* array,const std::vector<uint32_t>& values) {
      out<<"static const uint32_t "<<array<<"[] = {\n";
      for(size_t i=0;i<values.size();++i)
        out<<(i%12==0?"  ":" ")<<values[i]<<"u,"<<((i%12==11||i+1==values.size())?"\n":"");
      out<<"};\n";
    };
    if(!edges.empty())indices("trace_edges",edges);
    indices("trace_forms",forms);
    const auto entry="traceml_generated_"+std::string(name)+"_run";
    out<<"\nstatic const traceml_module_v1 trace_module = {\n"
       <<"  "<<TRACEML_ABI_V1<<"u, trace_nodes, sizeof(trace_nodes) / sizeof(trace_nodes[0]),\n"
       <<"  "<<(edges.empty()?"NULL, 0":"trace_edges, sizeof(trace_edges) / sizeof(trace_edges[0])")<<",\n"
       <<"  trace_forms, sizeof(trace_forms) / sizeof(trace_forms[0])\n};\n\n"
       <<"extern traceml_entry_v1 "<<entry<<";\n"
       <<"traceml_status "<<entry<<"(const int64_t *args, size_t count,\n"
       <<"    const traceml_options *options, int64_t *result, traceml_error *error) {\n"
       <<"  return traceml_run_v1(&trace_module, args, count, options, result, error);\n}\n";
    if(!out)throw Error{Error::Code::Internal,"failed to write TraceML C99 translation unit"};
    return out.str();
  }
};
}

Result<std::string> emit_c99(const Program& source,std::string_view name) {
  using Output=Result<std::string>;
  if(!valid_module(name))return Output::err({Error::Code::InvalidArgument,"expected an ASCII module identifier of at most 96 characters"});
  try {
    auto valid=verify(source);
    if(!valid)return Output::err(valid.error());
    if(source.forms.size()>Module::max_records)
      return Output::err({Error::Code::ResourceLimit,"TraceML C99 module exceeds form limit"});
    Module module;
    for(const auto& form:source.forms)module.forms.push_back(module.lower(*form));
    return Output::ok(module.render(name));
  }catch(const Error& e){return Output::err(e);}
   catch(const std::bad_alloc&){return Output::err({Error::Code::ResourceLimit,"TraceML C99 allocation failed"});}
}
}
