#pragma once
// An adapter from Nerotypical's existing constraint-checker demonstration AST.
// The oracle provides the *inferred* type of each source expression; it can
// be backed by a successful StructuralChecker result or another frontend.
// Calling the bridge does not itself prove the program well typed: run the
// existing type checker first and Verifier afterwards.
#include "Syntax.hpp"
#include "../Language.hpp"
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace moonstone::nerotypical::sequent {
class AstBridge {
  using Expr=moonstone::nerotypical::ast::Expr;
  using Kind=Expr::Kind;
  using TypeOf=std::function<Type(const Expr&)>;
  using FunctionTypeOf=std::function<Type(const ast::Function&)>;
  using StringValueOf=std::function<std::string(const Expr&)>;
  TypeOf type_of_;
  FunctionTypeOf function_type_of_;
  StringValueOf string_value_of_;
  Type ty(const Expr &e) const {return type_of_(e);}
  static std::string binary_name(ast::BinOp op) {
    switch(op) {
    case ast::BinOp::Add:return "$nt.add";
    case ast::BinOp::Sub:return "$nt.sub";
    case ast::BinOp::Mul:return "$nt.mul";
    case ast::BinOp::Div:return "$nt.div";
    case ast::BinOp::Lt:return "$nt.lt";
    case ast::BinOp::Le:return "$nt.le";
    case ast::BinOp::Gt:return "$nt.gt";
    case ast::BinOp::Ge:return "$nt.ge";
    case ast::BinOp::Eq:return "$nt.eq";
    case ast::BinOp::Ne:return "$nt.ne";
    case ast::BinOp::And:return "$nt.and";
    case ast::BinOp::Or:return "$nt.or";
    }
    throw std::invalid_argument("unrecognized Nerotypical binary operation");
  }
  static std::string unary_name(ast::UnOp op) {return op==ast::UnOp::Neg?"$nt.neg":"$nt.not";}
  TermPtr applications(TermPtr callee,const std::vector<Expr> &args,Type result) const {
    Kont k;
    for(const auto &arg:args)k.frames.push_back(Frame::apply(lower(arg)));
    return make::mu(result,make::cut(std::move(callee),std::move(k)));
  }
public:
  explicit AstBridge(TypeOf get_type,FunctionTypeOf get_function_type={},
                     StringValueOf get_string_value={}):
    type_of_(std::move(get_type)),function_type_of_(std::move(get_function_type)),
    string_value_of_(std::move(get_string_value)) {
    if(!type_of_)throw std::invalid_argument("missing inferred type oracle");
  }
  TermPtr lower(const Expr &e) const {
    switch(e.kind) {
    case Kind::IntLiteral:return make::integer(e.int_value);
    case Kind::FloatLiteral:return make::floating(e.float_value);
    case Kind::StringLiteral:
      if(!string_value_of_)throw std::invalid_argument("AST StringLiteral omits text; supply a literal-text callback");
      return make::string(string_value_of_(e));
    case Kind::BoolLiteral:return make::boolean(e.bool_value);
    case Kind::UnitLiteral:return make::unit();
    case Kind::Variable:return make::variable(e.name,ty(e));
    case Kind::Lambda: {
      if(e.children.size()!=1)throw std::invalid_argument("lambda requires one body");
      Type fun=ty(e);
      if(fun.tag!=Type::Tag::Arrow)throw std::invalid_argument("type oracle must supply function type");
      return make::lambda(e.name,fun.args.at(0),lower(e.children[0]));
    }
    case Kind::Apply: {
      if(e.children.size()!=2)throw std::invalid_argument("apply requires function and argument");
      Kont k; k.frames.push_back(Frame::apply(lower(e.children[1])));
      return make::mu(ty(e),make::cut(lower(e.children[0]),std::move(k)));
    }
    case Kind::Call: {
      Type ft=ty(e);
      for(auto it=e.children.rbegin();it!=e.children.rend();++it)ft=Type::arrow(ty(*it),ft);
      return applications(make::variable(e.name,ft),e.children,ty(e));
    }
    case Kind::Let: {
      if(e.children.size()!=2)throw std::invalid_argument("let requires value and body");
      Type bt=ty(e.children[0]);
      return make::mu(ty(e),make::let_value(e.name,bt,lower(e.children[0]),make::cut(lower(e.children[1]))));
    }
    case Kind::If: {
      if(e.children.size()!=3)throw std::invalid_argument("if requires condition and two branches");
      Kont k;
      k.frames.push_back(Frame::cases({make::on_bool(true,make::cut(lower(e.children[1]))),make::on_bool(false,make::cut(lower(e.children[2])))}));
      return make::mu(ty(e),make::cut(lower(e.children[0]),std::move(k)));
    }
    case Kind::Binary: {
      if(e.children.size()!=2)throw std::invalid_argument("binary op requires two operands");
      Type ft=Type::arrow(ty(e.children[0]),Type::arrow(ty(e.children[1]),ty(e)));
      return applications(make::variable(binary_name(ast::binary_op(e)),ft),e.children,ty(e));
    }
    case Kind::Unary: {
      if(e.children.size()!=1)throw std::invalid_argument("unary op requires one operand");
      Type ft=Type::arrow(ty(e.children[0]),ty(e));
      return applications(make::variable(unary_name(ast::unary_op(e)),ft),e.children,ty(e));
    }
    }
    throw std::invalid_argument("unknown Nerotypical AST node");
  }
  Module lower_program(const ast::Program &p) const {
    Module m;m.result_type=ty(p.body);m.entry=make::cut(lower(p.body));
    std::vector<ValueBinding> functions;
    for(const auto &fn:p.functions) {
      if(!function_type_of_)throw std::invalid_argument("function declarations require a function signature oracle");
      Type ft=function_type_of_(fn),cursor=ft;
      std::vector<Type> parameter_types;
      for(const auto &name:fn.params) {
        (void)name;
        if(cursor.tag!=Type::Tag::Arrow || cursor.args.size()!=2)
          throw std::invalid_argument("function signature has fewer arrows than declared parameters");
        parameter_types.push_back(cursor.args[0]);cursor=cursor.args[1];
      }
      auto value=lower(fn.body);
      if(value->type!=cursor)throw std::invalid_argument("function body type does not match signature for "+fn.name);
      for(std::size_t i=fn.params.size();i>0;--i)value=make::lambda(fn.params[i-1],parameter_types[i-1],value);
      if(value->type!=ft)throw std::logic_error("internal bridge error: curried signature mismatch");
      functions.push_back({fn.name,ft,std::move(value)});
    }
    if(!functions.empty())m.entry=make::let_rec(std::move(functions),m.entry);
    // Native operators are explicit typed globals. They must appear in the
    // source AST, not inferred from an arbitrary "$nt" free variable.
    std::map<std::string,Type> natives;
    auto collect=[&](auto &&self,const Expr &e)->void {
      if(e.kind==Kind::Binary && e.children.size()==2) {
        Type ft=Type::arrow(ty(e.children[0]),Type::arrow(ty(e.children[1]),ty(e)));
        const auto key=binary_name(ast::binary_op(e));
        if(natives.count(key) && natives.at(key)!=ft)throw std::invalid_argument("inconsistent native operator signature for "+key);
        natives[key]=ft;
      }
      if(e.kind==Kind::Unary && e.children.size()==1) {
        Type ft=Type::arrow(ty(e.children[0]),ty(e));
        const auto key=unary_name(ast::unary_op(e));
        if(natives.count(key) && natives.at(key)!=ft)throw std::invalid_argument("inconsistent native operator signature for "+key);
        natives[key]=ft;
      }
      for(const auto &child:e.children)self(self,child);
    };
    for(const auto &fn:p.functions)collect(collect,fn.body);
    collect(collect,p.body);
    for(auto &[name,t]:natives)m.globals.push_back({name,t});
    return m;
  }

};
} // namespace moonstone::nerotypical::sequent
