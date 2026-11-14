#include <nerotypical/sequent/SequentIR.hpp>
#include <cassert>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace moonstone::nerotypical::sequent;
namespace m=moonstone::nerotypical::sequent::make;
static void valid(const Module &p) {
  const auto v=Verifier{}.check(p);
  if(!v.ok()) {
    for(const auto &s:v.errors)std::cerr<<s<<'\n';
    throw std::runtime_error("verify failed");
  }
}
static std::int64_t integer(const Module &p) {
  valid(p); const auto value=Evaluator{}.run(p);assert(value.tag==Value::Tag::Int);return value.integer;
}
static bool boolean(const Module &p) {
  valid(p); const auto value=Evaluator{}.run(p);assert(value.tag==Value::Tag::Bool);return value.boolean;
}
static void basic() {
  Type i=Type::integer();
  auto identity=m::lambda("x",i,m::variable("x",i));
  Module p{{},{},m::cut(identity,Kont{{Frame::apply(m::integer(42))}}),i};
  assert(integer(p)==42);
  RewriteStats st; p.entry=simplify(p.entry,&st);
  assert(st.changed && !st.reached_limit);
  assert(integer(p)==42);
}
static void mu_case_join() {
  Type i=Type::integer(),b=Type::boolean();
  auto join=m::let_join("j",{{"x",i}},m::cut(m::variable("x",i)),m::jump("j",{m::integer(17)}));
  auto v=m::mu(i,join);
  Kont k{{Frame::cases({m::otherwise("z",i,m::cut(m::boolean(true)))})}};
  Module p{{},{},m::cut(v,k),b};
  assert(boolean(p));
  p.entry=simplify(p.entry);
  valid(p);
  assert(p.entry->tag==Command::Tag::LetJoin);
  assert(p.entry->next->tag==Command::Tag::Jump);
  assert(boolean(p));
}
static void constructor_case() {
  Type i=Type::integer(),opt=Type::data("MaybeInt");
  Module p;
  p.result_type=i;
  p.constructors={{"Just",opt,{i}},{"Nothing",opt,{}}};
  auto case_k=Kont{{Frame::cases({
    m::on_constructor("Just",{{"a",i}},m::cut(m::variable("a",i))),
    m::on_constructor("Nothing",{},m::cut(m::integer(0)))
  })}};
  p.entry=m::cut(m::constructor("Just",opt,{m::integer(8)}),case_k);
  assert(integer(p)==8);
  p.entry=simplify(p.entry);
  assert(integer(p)==8);
}
static void type_application() {
  Type a=Type::var("a"),i=Type::integer();
  auto polymorphic=m::type_lambda("a",m::lambda("x",a,m::variable("x",a)));
  Module p{{},{},m::cut(polymorphic,Kont{{Frame::instantiate(i),Frame::apply(m::integer(99))}}),i};
  assert(integer(p)==99);
}
static void contification() {
  Type i=Type::integer();
  auto f=m::lambda("n",i,m::mu(i,m::cut(m::variable("n",i))));
  Kont args{{Frame::apply(m::integer(24))}};
  Module p{{},{},m::let_value("f",f->type,f,m::cut(m::variable("f",f->type),args)),i};
  assert(integer(p)==24);
  bool changed=false;p.entry=contify(p.entry,i,&changed);
  assert(changed && p.entry->tag==Command::Tag::LetJoin);
  assert(p.entry->next->tag==Command::Tag::Jump);
  assert(integer(p)==24);
}
static void no_contify_escaping_function() {
  Type i=Type::integer();
  auto f=m::lambda("n",i,m::mu(i,m::cut(m::variable("n",i))));
  Module p{{},{},m::let_value("f",f->type,f,m::let_value("g",f->type,m::variable("f",f->type),
    m::cut(m::variable("g",f->type),Kont{{Frame::apply(m::integer(4))}}))),i};
  valid(p);
  bool changed=true;auto rewritten=contify(p.entry,i,&changed);
  assert(!changed && rewritten->tag==Command::Tag::LetValue);
  assert(integer(p)==4);
}
static void lazy_unused_binding() {
  Type i=Type::integer();
  auto loop=m::let_join("loop",{},m::jump("loop",{}),m::jump("loop",{}));
  Module p{{},{},m::let_value("unused",i,m::mu(i,loop),m::cut(m::integer(12))),i};
  assert(integer(p)==12);
  p.entry=simplify(p.entry);
  assert(p.entry->tag==Command::Tag::Cut);
  assert(integer(p)==12);
}
static void reject_label_escape() {
  Type i=Type::integer(),fun=Type::arrow(i,i);
  auto escape=m::lambda("arg",i,m::mu(i,m::jump("outer",{m::variable("arg",i)})));
  Module p{{},{},m::let_join("outer",{{"x",i}},m::cut(m::variable("x",i)),m::cut(escape,Kont{{Frame::apply(m::integer(4))}})),i};
  auto v=Verifier{}.check(p);
  assert(!v.ok());
  bool saw=false;
  for(const auto &e:v.errors)if(e.find("unbound jump label 'outer'")!=std::string::npos)saw=true;
  assert(saw);
}
static void invalid_type() {
  Type i=Type::integer(),b=Type::boolean();
  Module p{{},{},m::cut(m::integer(2),Kont{{Frame::apply(m::boolean(true))}}),i};
  assert(!Verifier{}.check(p).ok());
  p.entry=m::cut(m::boolean(false));
  assert(!Verifier{}.check(p).ok());
}
static void join_reuse() {
  Type i=Type::integer();
  Module p{{},{},m::let_join("again",{{"x",i}},m::cut(m::variable("x",i)),
    m::jump("again",{m::integer(6)})),i};
  assert(integer(p)==6);
  auto printed=print_module(p);
  assert(printed.find("jump again 6")!=std::string::npos);
}
static void recursive_values_and_primitives() {
  Type i=Type::integer(),b=Type::boolean();
  Type eq=Type::arrow(i,Type::arrow(i,b));
  Type sub=Type::arrow(i,Type::arrow(i,i));
  Type fn=Type::arrow(i,i);
  auto dec=m::mu(i,m::cut(m::variable("$nt.sub",sub),Kont{{
      Frame::apply(m::variable("n",i)),Frame::apply(m::integer(1))}}));
  auto again=m::cut(m::variable("count",fn),Kont{{Frame::apply(dec)}});
  auto cond=m::cut(m::variable("$nt.eq",eq),Kont{{
      Frame::apply(m::variable("n",i)),Frame::apply(m::integer(0)),
      Frame::cases({m::on_bool(true,m::cut(m::integer(0))),m::on_bool(false,again)})}});
  auto recursive=m::lambda("n",i,m::mu(i,cond));
  Module p;
  p.result_type=i;
  p.globals={{"$nt.eq",eq},{"$nt.sub",sub}};
  p.entry=m::let_rec({{"count",fn,recursive}},m::cut(m::variable("count",fn),
    Kont{{Frame::apply(m::integer(3))}}));
  assert(integer(p)==0);
  auto o=optimize_checked(p);
  assert(integer(o)==0);
}
static void string_float_operators() {
  Type f=Type::floating(),i=Type::integer(),s=Type::string();
  Type add=Type::arrow(f,Type::arrow(f,f));
  Module p{{},{ {"$nt.add",add} },m::cut(m::variable("$nt.add",add),
    Kont{{Frame::apply(m::floating(1.5)),Frame::apply(m::floating(2.25))}}),f};
  valid(p);
  auto result=Evaluator{}.run(p);assert(result.tag==Value::Tag::Float && result.floating==3.75);
  Module q{{},{},m::cut(m::string("hello")),s};
  valid(q);assert(Evaluator{}.run(q).text=="hello");
}
static void dead_join_and_metrics() {
  Type i=Type::integer();
  Module p{{},{},m::let_join("unused",{{"x",i}},m::cut(m::variable("x",i)),m::cut(m::integer(21))),i};
  assert(inspect(p).join_bindings==1);
  auto dot=to_dot(p);assert(dot.find("join unused")!=std::string::npos);
  auto result=optimize_checked(p);
  assert(inspect(result).join_bindings==0);
  assert(integer(result)==21);
}
static void evaluator_error_paths() {
  Type i=Type::integer(),b=Type::boolean();
  Type add=Type::arrow(i,Type::arrow(i,i));
  Module bad{{},{ {"$nt.add",add} },m::cut(m::variable("$nt.add",add),
    Kont{{Frame::apply(m::integer(std::numeric_limits<std::int64_t>::max())),Frame::apply(m::integer(1))}}),i};
  valid(bad);
  bool caught=false;
  try {Evaluator{}.run(bad);}catch(const std::overflow_error &){caught=true;}
  assert(caught);
  Type equal=Type::arrow(i,Type::arrow(i,b));
  Module large{{},{ {"$nt.eq",equal} },m::cut(m::variable("$nt.eq",equal),
    Kont{{Frame::apply(m::integer(9007199254740993LL)),Frame::apply(m::integer(9007199254740992LL))}}),b};
  assert(!boolean(large));
  auto loop=m::let_join("spin",{},m::jump("spin",{}),m::jump("spin",{}));
  Module nonterm{{},{},loop,i};
  caught=false;
  try {Evaluator{100}.run(nonterm);}catch(const std::runtime_error &){caught=true;}
  assert(caught);
}
static void nested_mu_scope_substitution() {
  Type i=Type::integer(),b=Type::boolean();
  auto inner=m::mu(i,m::cut(m::integer(5)));
  auto body=m::let_value("deferred",i,inner,m::cut(m::integer(17)));
  auto outside=make::mu(i,body);
  Kont extra{{Frame::cases({m::otherwise("z",i,m::cut(m::boolean(true)))})}};
  Module p{{},{},m::cut(outside,extra),b};
  assert(boolean(p));
  auto optimized=optimize_checked(p);
  assert(boolean(optimized));
}
int main() {
  basic();mu_case_join();constructor_case();type_application();contification();
  no_contify_escaping_function();lazy_unused_binding();reject_label_escape();
  invalid_type();join_reuse();recursive_values_and_primitives();
  string_float_operators();dead_join_and_metrics();evaluator_error_paths();
  nested_mu_scope_substitution();
  std::cout<<"15 Sequent IR test groups OK\n";
}
