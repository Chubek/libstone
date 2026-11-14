#pragma once
// Sequent Core-inspired, pure, typed command IR for Moonstone/Nerotypical.
// Independent of Satie so the frontend/optimizer can be built standalone.
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace moonstone::nerotypical::sequent {
struct Type {
  enum class Tag { Int, Float, Bool, String, Unit, Var, Arrow, Forall, Data } tag = Tag::Unit;
  std::string name; // Var/Forall binder/Data name
  std::vector<Type> args;
  static Type integer() { return {Tag::Int,{}, {}}; }
  static Type floating() { return {Tag::Float,{}, {}}; }
  static Type string() { return {Tag::String,{}, {}}; }
  static Type boolean() { return {Tag::Bool,{}, {}}; }
  static Type unit() { return {Tag::Unit,{}, {}}; }
  static Type var(std::string n) { return {Tag::Var,std::move(n),{}}; }
  static Type arrow(Type a, Type b) { return {Tag::Arrow,{}, {std::move(a),std::move(b)}}; }
  static Type forall(std::string n, Type a) { return {Tag::Forall,std::move(n),{std::move(a)}}; }
  static Type data(std::string n, std::vector<Type> a={}) { return {Tag::Data,std::move(n),std::move(a)}; }
};
inline bool operator==(const Type &a, const Type &b) {
  return a.tag==b.tag && a.name==b.name && a.args==b.args;
}
inline bool operator!=(const Type &a, const Type &b) { return !(a==b); }
inline std::string type_string(const Type &t) {
  switch(t.tag) {
  case Type::Tag::Int: return "Int";
  case Type::Tag::Float: return "Float";
  case Type::Tag::String: return "String";
  case Type::Tag::Bool: return "Bool";
  case Type::Tag::Unit: return "Unit";
  case Type::Tag::Var: return t.name;
  case Type::Tag::Arrow: return "("+type_string(t.args.at(0))+" -> "+type_string(t.args.at(1))+")";
  case Type::Tag::Forall: return "(forall "+t.name+". "+type_string(t.args.at(0))+")";
  case Type::Tag::Data: {
    std::string s=t.name;
    for(const auto &a:t.args) s+=" "+type_string(a);
    return s;
  }
  }
  return "<?>";
}
// Capture-avoiding for types: rename nested bound variables on collision.
inline Type substitute_type(const Type &t, const std::string &v, const Type &with) {
  if(t.tag==Type::Tag::Var && t.name==v) return with;
  if(t.tag==Type::Tag::Forall && t.name==v) return t;
  Type r=t;
  if(t.tag==Type::Tag::Forall) {
    // Caller must alpha-rename forall binders before a possibly capturing substitution.
    // Reject collisions rather than silently capture.
    auto contains=[&](auto &&self,const Type &x)->bool {
      if(x.tag==Type::Tag::Var && x.name==t.name) return true;
      for(auto &a:x.args) if(self(self,a)) return true;
      return false;
    };
    if(contains(contains,with)) throw std::invalid_argument("type substitution requires alpha-renaming binder "+t.name);
  }
  for(auto &a:r.args) a=substitute_type(a,v,with);
  return r;
}

struct Term; struct Command;
using TermPtr=std::shared_ptr<Term>;
using CmdPtr=std::shared_ptr<Command>;
struct Parameter {std::string name; Type type;};
struct Alternative {
  enum class Tag { Constructor, True, False, Default } tag=Tag::Default;
  std::string constructor;
  std::vector<Parameter> binders;
  CmdPtr body;
};
struct Frame {
  enum class Tag { Apply, TypeApply, Case } tag=Tag::Apply;
  TermPtr argument; Type type_argument;
  std::vector<Alternative> alternatives;
  static Frame apply(TermPtr a) { Frame f; f.tag=Tag::Apply; f.argument=std::move(a); return f; }
  static Frame instantiate(Type a) { Frame f; f.tag=Tag::TypeApply; f.type_argument=std::move(a); return f; }
  static Frame cases(std::vector<Alternative> a) { Frame f; f.tag=Tag::Case; f.alternatives=std::move(a); return f; }
};
// The final Ret continuation is implicit. Case must be the last frame.
struct Kont {std::vector<Frame> frames;};
struct Term {
  enum class Tag { Var, Int, Float, Bool, String, Unit, Lambda, TypeLambda, Constructor, Mu } tag=Tag::Unit;
  Type type; std::string name; // variable, lambda binder, type binder, constructor name
  std::int64_t integer=0;double floating=0; bool boolean=false;std::string text;
  std::vector<TermPtr> operands; // constructor fields; lambda body is operands[0]
  CmdPtr command; // Mu body
};
struct ValueBinding {std::string name;Type type;TermPtr value;};
struct Command {
  enum class Tag { Cut, Jump, LetValue, LetRec, LetJoin } tag=Tag::Cut;
  TermPtr term; Kont kont; // Cut
  std::string name; // Jump target / Let binding
  std::vector<Type> type_arguments; std::vector<TermPtr> arguments; // Jump
  Type bound_type; TermPtr value; // LetValue
  std::vector<ValueBinding> recursive_values; // LetRec: mutually recursive value binders
  std::vector<std::string> type_parameters; std::vector<Parameter> parameters; // LetJoin
  CmdPtr definition; CmdPtr next; // LetJoin definition / both Let bodies
};
namespace make {
inline TermPtr variable(std::string n,Type t={}) {auto p=std::make_shared<Term>();p->tag=Term::Tag::Var;p->name=std::move(n);p->type=std::move(t);return p;}
inline TermPtr integer(std::int64_t n) {auto p=std::make_shared<Term>();p->tag=Term::Tag::Int;p->integer=n;p->type=Type::integer();return p;}
inline TermPtr floating(double n) {auto p=std::make_shared<Term>();p->tag=Term::Tag::Float;p->floating=n;p->type=Type::floating();return p;}
inline TermPtr string(std::string n) {auto p=std::make_shared<Term>();p->tag=Term::Tag::String;p->text=std::move(n);p->type=Type::string();return p;}
inline TermPtr boolean(bool n) {auto p=std::make_shared<Term>();p->tag=Term::Tag::Bool;p->boolean=n;p->type=Type::boolean();return p;}
inline TermPtr unit() {auto p=std::make_shared<Term>();p->tag=Term::Tag::Unit;p->type=Type::unit();return p;}
inline TermPtr lambda(std::string n,Type arg,TermPtr body) {auto p=std::make_shared<Term>();p->tag=Term::Tag::Lambda;p->name=std::move(n);p->type=Type::arrow(arg,body->type);p->operands={std::move(body)};return p;}
inline TermPtr type_lambda(std::string n,TermPtr body) {auto p=std::make_shared<Term>();p->tag=Term::Tag::TypeLambda;p->name=std::move(n);p->type=Type::forall(p->name,body->type);p->operands={std::move(body)};return p;}
inline TermPtr constructor(std::string n,Type t,std::vector<TermPtr> args={}) {auto p=std::make_shared<Term>();p->tag=Term::Tag::Constructor;p->name=std::move(n);p->type=std::move(t);p->operands=std::move(args);return p;}
inline TermPtr mu(Type t,CmdPtr c) {auto p=std::make_shared<Term>();p->tag=Term::Tag::Mu;p->type=std::move(t);p->command=std::move(c);return p;}
inline CmdPtr cut(TermPtr v,Kont k={}) {auto c=std::make_shared<Command>();c->tag=Command::Tag::Cut;c->term=std::move(v);c->kont=std::move(k);return c;}
inline CmdPtr jump(std::string j,std::vector<TermPtr> args,std::vector<Type> ts={}) {auto c=std::make_shared<Command>();c->tag=Command::Tag::Jump;c->name=std::move(j);c->arguments=std::move(args);c->type_arguments=std::move(ts);return c;}
inline CmdPtr let_value(std::string n,Type t,TermPtr v,CmdPtr next) {auto c=std::make_shared<Command>();c->tag=Command::Tag::LetValue;c->name=std::move(n);c->bound_type=std::move(t);c->value=std::move(v);c->next=std::move(next);return c;}
inline CmdPtr let_rec(std::vector<ValueBinding> bindings,CmdPtr next) {auto c=std::make_shared<Command>();c->tag=Command::Tag::LetRec;c->recursive_values=std::move(bindings);c->next=std::move(next);return c;}
inline CmdPtr let_join(std::string j,std::vector<Parameter> ps,CmdPtr code,CmdPtr next,std::vector<std::string> tps={}) {auto c=std::make_shared<Command>();c->tag=Command::Tag::LetJoin;c->name=std::move(j);c->parameters=std::move(ps);c->type_parameters=std::move(tps);c->definition=std::move(code);c->next=std::move(next);return c;}
inline Alternative on_constructor(std::string n,std::vector<Parameter> p,CmdPtr c) {return {Alternative::Tag::Constructor,std::move(n),std::move(p),std::move(c)};}
inline Alternative on_bool(bool b,CmdPtr c) {return {b?Alternative::Tag::True:Alternative::Tag::False,{}, {},std::move(c)};}
inline Alternative otherwise(std::string n,Type t,CmdPtr c) {return {Alternative::Tag::Default,{},{{std::move(n),std::move(t)}},std::move(c)};}
}
struct DataConstructor {std::string name; Type result; std::vector<Type> fields;};
struct Module {std::vector<DataConstructor> constructors; std::vector<Parameter> globals; CmdPtr entry; Type result_type=Type::unit();};
} // namespace moonstone::nerotypical::sequent
