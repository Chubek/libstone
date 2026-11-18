#include "traceml.hpp"
extern "C" {
#include "TraceSourceAST.h"
traceml_ast_document_t traceml_parse_source(const char*,traceml_ast_arena*,size_t*);
}
#include <charconv>
#include <mutex>

namespace limestone::traceml::detail {
namespace {
struct Arena {
  traceml_ast_arena data;
  Arena() {traceml_ast_arena_init(&data);}
  ~Arena() {traceml_ast_arena_destroy(&data);}
};
struct Locations {
  std::vector<size_t> lines{0};
  std::pair<uint32_t,uint32_t> at(size_t offset) const {
    const size_t line=std::upper_bound(lines.begin(),lines.end(),offset)-lines.begin();
    return {static_cast<uint32_t>(line),static_cast<uint32_t>(offset-lines[line-1]+1)};
  }
  Error error(size_t offset,std::string message,Error::Code code=Error::Code::Parse) const {
    auto [line,column]=at(offset);
    return {code,"TraceML "+std::to_string(line)+":"+std::to_string(column)+": "+std::move(message)};
  }
};
constexpr bool decimal(char c) {return c>='0'&&c<='9';}
std::shared_ptr<Expr> convert(const traceml_ast_expr_node* node,const Locations& locations) {
  auto expr=std::make_shared<Expr>();
  expr->offset=node->offset;
  std::tie(expr->line,expr->column)=locations.at(node->offset);
  if(node->kind==traceml_ast_List_kind) {
    expr->kind=Expr::Kind::Apply;
    const auto list=node->v.List.items;
    expr->children.reserve(list.length);
    for(size_t i=0;i<list.length;++i)expr->children.push_back(convert(list.items[i],locations));
  }else {
    const auto atom=node->v.Atom.spelling;
    const std::string_view text{atom.data,atom.length};
    auto digits=text;
    if(digits.starts_with('-'))digits.remove_prefix(1);
    if(!digits.empty()&&std::all_of(digits.begin(),digits.end(),decimal)) {
      expr->kind=Expr::Kind::Integer;
      auto parsed=std::from_chars(text.data(),text.data()+text.size(),expr->integer);
      if(parsed.ec!=std::errc{})
        throw locations.error(node->offset,"integer out of range");
    }else {
      if(decimal(text.front()))throw locations.error(node->offset,"invalid symbol");
      expr->kind=Expr::Kind::Symbol;
      expr->atom=text;
    }
  }
  return expr;
}
}

Result<Program> parse_document(std::string_view text) {
  try {
    if(text.size()>1024*1024)return Result<Program>::err({Error::Code::ResourceLimit,"TraceML input exceeds 1 MiB"});
    Locations locations;
    size_t depth=0;
    bool comment=false;
    for(size_t i=0;i<text.size();++i) {
      const char c=text[i];
      if(c=='\0')throw locations.error(i,"NUL in source");
      if(c=='\n'){locations.lines.push_back(i+1);comment=false;}
      if(comment)continue;
      if(c==';')comment=true;
      else if(c=='('&&++depth>256)throw locations.error(i,"nesting limit reached",Error::Code::ResourceLimit);
      else if(c==')'&&depth)--depth;
    }
    const std::string input(text);
    Arena arena;
    size_t offset=0;
    traceml_ast_document_t parsed;
    {
      static std::mutex mutex;
      std::lock_guard lock(mutex);
      parsed=traceml_parse_source(input.c_str(),&arena.data,&offset);
    }
    if(!parsed)throw locations.error(offset,arena.data.error?"syntax allocation failed":"invalid syntax",
                                      arena.data.error?Error::Code::ResourceLimit:Error::Code::Parse);
    Program out;
    out.forms.reserve(parsed->forms.length);
    for(size_t i=0;i<parsed->forms.length;++i)out.forms.push_back(convert(parsed->forms.items[i],locations));
    return Result<Program>::ok(std::move(out));
  }catch(const Error& e){return Result<Program>::err(e);}
   catch(const std::bad_alloc&){return Result<Program>::err({Error::Code::ResourceLimit,"TraceML syntax allocation failed"});}
}
}
