#pragma once
#include "Verify.hpp"
#include <functional>
#include <limits>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace moonstone::nerotypical::sequent {
// Reference *call-by-need* interpreter (not a native code generator).
// It memoizes forced variables, keeps jump targets lexical, and uses one
// dynamic return continuation for each Mu computation.
struct Cell; struct Environment;
using Env=std::shared_ptr<Environment>;
struct Value {
  enum class Tag { Int, Float, Bool, String, Unit, Lambda, TypeLambda, Constructor, Native } tag=Tag::Unit;
  std::int64_t integer=0;double floating=0;bool boolean=false;std::string text;
  std::string constructor;TermPtr closure_term;Env closure;
  std::vector<std::shared_ptr<Cell>> fields;
  std::string native_name;std::vector<std::shared_ptr<Cell>> native_args;
  static Value integer_value(std::int64_t i) {Value v;v.tag=Tag::Int;v.integer=i;return v;}
  static Value boolean_value(bool b) {Value v;v.tag=Tag::Bool;v.boolean=b;return v;}
  static Value float_value(double f) {Value v;v.tag=Tag::Float;v.floating=f;return v;}
  static Value string_value(std::string s) {Value v;v.tag=Tag::String;v.text=std::move(s);return v;}
};
struct Cell {TermPtr expression;Env env;std::weak_ptr<Environment> recursive_env;bool recursive=false;std::optional<Value> cached;bool busy=false;};
struct Environment {std::map<std::string,std::shared_ptr<Cell>> values;};
inline std::string value_string(const Value &v) {
  switch(v.tag) {
  case Value::Tag::Int:return std::to_string(v.integer);
  case Value::Tag::Float:return std::to_string(v.floating);
  case Value::Tag::String:return v.text;
  case Value::Tag::Bool:return v.boolean?"True":"False";
  case Value::Tag::Unit:return "()";
  case Value::Tag::Lambda:return "<lambda>";
  case Value::Tag::TypeLambda:return "<type-lambda>";
  case Value::Tag::Constructor:return v.constructor;
  case Value::Tag::Native:return "<native:"+v.native_name+">";
  }
  return "<?>";
}
struct JoinClosure;
struct JumpEnvironment {std::map<std::string,std::shared_ptr<JoinClosure>> joins;};
using JEnv=std::shared_ptr<JumpEnvironment>;
using Return=std::function<Value(Value)>;
struct JoinClosure {std::vector<Parameter> parameters;CmdPtr definition;Env env;std::weak_ptr<JumpEnvironment> joins;Return result;};
class Evaluator {
  std::size_t steps_=0,max_steps_;
  std::vector<std::weak_ptr<Cell>> recursive_cells_;
  void clear_recursive_caches() noexcept {
    for(auto &weak:recursive_cells_)if(auto cell=weak.lock())cell->cached.reset();
    recursive_cells_.clear();
  }
  void tick() {if(++steps_>max_steps_)throw std::runtime_error("sequent evaluator: step/fuel budget exhausted");}
  static Env child(const Env &env) {return std::make_shared<Environment>(*env);}
  static JEnv child(const JEnv &env) {return std::make_shared<JumpEnvironment>(*env);}
  static std::shared_ptr<Cell> thunk(TermPtr v,Env env) {auto c=std::make_shared<Cell>();c->expression=std::move(v);c->env=std::move(env);return c;}
  static std::shared_ptr<Cell> recursive_thunk(TermPtr v,const Env &env) {
    auto c=std::make_shared<Cell>();c->expression=std::move(v);
    c->recursive_env=env;c->recursive=true;return c;
  }
  Value force(const std::shared_ptr<Cell> &cell) {
    tick();
    if(cell->cached)return *cell->cached;
    if(cell->busy)throw std::runtime_error("sequent evaluator: recursive thunk black hole");
    if(!cell->expression)throw std::runtime_error("sequent evaluator: uninitialized thunk");
    cell->busy=true;
    try {
      Env env=cell->recursive ? cell->recursive_env.lock() : cell->env;
      if(!env)throw std::runtime_error("sequent evaluator: expired recursive environment");
      Value v=eval_term(cell->expression,env,[](Value r){return r;});
      cell->cached=v;cell->busy=false;return v;
    } catch(...) {cell->busy=false;throw;}
  }
  Value eval_term(const TermPtr &t,const Env &env,const Return &k) {
    tick();if(!t)throw std::runtime_error("sequent evaluator: null term");
    switch(t->tag) {
    case Term::Tag::Var: {
      auto it=env->values.find(t->name);
      if(it==env->values.end())throw std::runtime_error("sequent evaluator: unknown value "+t->name);
      return k(force(it->second));
    }
    case Term::Tag::Int:return k(Value::integer_value(t->integer));
    case Term::Tag::Float:return k(Value::float_value(t->floating));
    case Term::Tag::String:return k(Value::string_value(t->text));
    case Term::Tag::Bool:return k(Value::boolean_value(t->boolean));
    case Term::Tag::Unit:return k(Value{});
    case Term::Tag::Lambda:
    case Term::Tag::TypeLambda: {
      Value v;v.tag=(t->tag==Term::Tag::Lambda?Value::Tag::Lambda:Value::Tag::TypeLambda);
      v.closure_term=t;v.closure=env;return k(std::move(v));
    }
    case Term::Tag::Constructor: {
      Value v;v.tag=Value::Tag::Constructor;v.constructor=t->name;
      for(const auto &a:t->operands)v.fields.push_back(thunk(a,env));
      return k(std::move(v));
    }
    case Term::Tag::Mu:
      // A computation creates its own lexical join namespace, but its
      // dynamic exit is the supplied consuming continuation.
      return eval_cmd(t->command,env,std::make_shared<JumpEnvironment>(),k);
    }
    throw std::runtime_error("sequent evaluator: invalid term tag");
  }
  static bool is_numeric(const Value &v) {return v.tag==Value::Tag::Int || v.tag==Value::Tag::Float;}
  static double as_double(const Value &v) {return v.tag==Value::Tag::Float?v.floating:static_cast<double>(v.integer);}
  static std::int64_t checked_add(std::int64_t a,std::int64_t b) {
    constexpr auto hi=std::numeric_limits<std::int64_t>::max();
    constexpr auto lo=std::numeric_limits<std::int64_t>::min();
    if((b>0 && a>hi-b)||(b<0 && a<lo-b))throw std::overflow_error("integer addition overflow");
    return a+b;
  }
  static std::int64_t checked_sub(std::int64_t a,std::int64_t b) {
    constexpr auto hi=std::numeric_limits<std::int64_t>::max();
    constexpr auto lo=std::numeric_limits<std::int64_t>::min();
    if((b<0 && a>hi+b)||(b>0 && a<lo+b))throw std::overflow_error("integer subtraction overflow");
    return a-b;
  }
  static std::int64_t checked_mul(std::int64_t a,std::int64_t b) {
    constexpr auto hi=std::numeric_limits<std::int64_t>::max();
    constexpr auto lo=std::numeric_limits<std::int64_t>::min();
    if(a==0 || b==0)return 0;
    if((a==-1 && b==lo)||(b==-1 && a==lo))throw std::overflow_error("integer multiplication overflow");
    if(a>0) {
      if(b>0 && a>hi/b)throw std::overflow_error("integer multiplication overflow");
      if(b<0 && b<lo/a)throw std::overflow_error("integer multiplication overflow");
    } else {
      if(b>0 && a<lo/b)throw std::overflow_error("integer multiplication overflow");
      if(b<0 && a<hi/b)throw std::overflow_error("integer multiplication overflow");
    }
    return a*b;
  }
  Value apply_native(Value native) {
    const auto &name=native.native_name;
    const bool unary=(name=="$nt.neg"||name=="$nt.not");
    const std::size_t expected=unary?1:2;
    if(native.native_args.size()<expected)return native;
    if(native.native_args.size()!=expected)throw std::runtime_error("primitive arity error");
    Value a=force(native.native_args.at(0));
    if(name=="$nt.neg") {
      if(a.tag==Value::Tag::Float)return Value::float_value(-a.floating);
      if(a.tag!=Value::Tag::Int || a.integer==std::numeric_limits<std::int64_t>::min())throw std::runtime_error("invalid/overflow negation");
      return Value::integer_value(-a.integer);
    }
    if(name=="$nt.not") {
      if(a.tag!=Value::Tag::Bool)throw std::runtime_error("invalid Boolean negation");
      return Value::boolean_value(!a.boolean);
    }
    if(name=="$nt.and" || name=="$nt.or") {
      if(a.tag!=Value::Tag::Bool)throw std::runtime_error("invalid Boolean operand");
      if(name=="$nt.and" && !a.boolean)return Value::boolean_value(false);
      if(name=="$nt.or" && a.boolean)return Value::boolean_value(true);
      Value b=force(native.native_args.at(1));
      if(b.tag!=Value::Tag::Bool)throw std::runtime_error("invalid Boolean operand");
      return Value::boolean_value(name=="$nt.and"?(a.boolean&&b.boolean):(a.boolean||b.boolean));
    }
    Value b=force(native.native_args.at(1));
    if(name=="$nt.eq"||name=="$nt.ne") {
      bool equal=false;
      if(a.tag==Value::Tag::Int && b.tag==Value::Tag::Int)equal=a.integer==b.integer;
      else if(is_numeric(a)&&is_numeric(b))equal=as_double(a)==as_double(b);
      else if(a.tag==b.tag) {
        if(a.tag==Value::Tag::Bool)equal=a.boolean==b.boolean;
        else if(a.tag==Value::Tag::Unit)equal=true;
        else if(a.tag==Value::Tag::String)equal=a.text==b.text;
      }
      return Value::boolean_value(name=="$nt.eq"?equal:!equal);
    }
    if(!is_numeric(a)||!is_numeric(b))throw std::runtime_error("primitive expects numbers");
    double x=as_double(a),y=as_double(b);
    if(name=="$nt.lt")return Value::boolean_value(x<y);
    if(name=="$nt.le")return Value::boolean_value(x<=y);
    if(name=="$nt.gt")return Value::boolean_value(x>y);
    if(name=="$nt.ge")return Value::boolean_value(x>=y);
    if(name=="$nt.div" && y==0)throw std::runtime_error("division by zero");
    if(a.tag==Value::Tag::Float||b.tag==Value::Tag::Float) {
      if(name=="$nt.add")return Value::float_value(x+y);
      if(name=="$nt.sub")return Value::float_value(x-y);
      if(name=="$nt.mul")return Value::float_value(x*y);
      if(name=="$nt.div")return Value::float_value(x/y);
    } else {
      if(name=="$nt.add")return Value::integer_value(checked_add(a.integer,b.integer));
      if(name=="$nt.sub")return Value::integer_value(checked_sub(a.integer,b.integer));
      if(name=="$nt.mul")return Value::integer_value(checked_mul(a.integer,b.integer));
      if(name=="$nt.div") {
        if(a.integer==std::numeric_limits<std::int64_t>::min()&&b.integer==-1)throw std::overflow_error("integer division overflow");
        return Value::integer_value(a.integer/b.integer);
      }
    }
    throw std::runtime_error("unknown native operation "+name);
  }
  Value eval_frames(Value v,const Kont &kont,std::size_t i,const Env &env,const JEnv &joins,const Return &ret) {
    tick();
    if(i==kont.frames.size())return ret(std::move(v));
    const Frame &f=kont.frames[i];
    if(f.tag==Frame::Tag::Apply) {
      if(v.tag==Value::Tag::Native) {
        v.native_args.push_back(thunk(f.argument,env));
        Value result=apply_native(std::move(v));
        return eval_frames(std::move(result),kont,i+1,env,joins,ret);
      }
      if(v.tag!=Value::Tag::Lambda)throw std::runtime_error("sequent evaluator: apply non-function");
      auto scope=child(v.closure);
      scope->values[v.closure_term->name]=thunk(f.argument,env);
      return eval_term(v.closure_term->operands.at(0),scope,[&](Value result){return eval_frames(std::move(result),kont,i+1,env,joins,ret);});
    }
    if(f.tag==Frame::Tag::TypeApply) {
      if(v.tag!=Value::Tag::TypeLambda)throw std::runtime_error("sequent evaluator: type-apply non-polymorphic term");
      return eval_term(v.closure_term->operands.at(0),v.closure,[&](Value result){return eval_frames(std::move(result),kont,i+1,env,joins,ret);});
    }
    if(f.tag==Frame::Tag::Case) {
      for(const auto &a:f.alternatives) {
        bool found=a.tag==Alternative::Tag::Default ||
          (a.tag==Alternative::Tag::True && v.tag==Value::Tag::Bool && v.boolean) ||
          (a.tag==Alternative::Tag::False && v.tag==Value::Tag::Bool && !v.boolean) ||
          (a.tag==Alternative::Tag::Constructor && v.tag==Value::Tag::Constructor && a.constructor==v.constructor);
        if(!found)continue;
        Env scope=child(env);
        if(a.tag==Alternative::Tag::Default) {
          auto cell=std::make_shared<Cell>();cell->cached=v;
          scope->values[a.binders.at(0).name]=cell;
        } else if(a.tag==Alternative::Tag::Constructor) {
          for(std::size_t j=0;j<a.binders.size();++j)scope->values[a.binders[j].name]=v.fields.at(j);
        }
        return eval_cmd(a.body,scope,joins,ret);
      }
      throw std::runtime_error("sequent evaluator: non-exhaustive case");
    }
    throw std::runtime_error("sequent evaluator: invalid frame tag");
  }
  Value eval_cmd(const CmdPtr &cmd,const Env &env,const JEnv &joins,const Return &ret) {
    tick();if(!cmd)throw std::runtime_error("sequent evaluator: null command");
    switch(cmd->tag) {
    case Command::Tag::Cut:
      return eval_term(cmd->term,env,[&](Value v){return eval_frames(std::move(v),cmd->kont,0,env,joins,ret);});
    case Command::Tag::LetValue: {
      auto scope=child(env);
      scope->values[cmd->name]=thunk(cmd->value,env);
      return eval_cmd(cmd->next,scope,joins,ret);
    }
    case Command::Tag::LetRec: {
      auto scope=child(env);
      for(const auto &bind:cmd->recursive_values) {
        auto cell=recursive_thunk(bind.value,scope);
        recursive_cells_.push_back(cell);
        scope->values[bind.name]=std::move(cell);
      }
      return eval_cmd(cmd->next,scope,joins,ret);
    }
    case Command::Tag::LetJoin: {
      auto scope=child(joins);
      auto j=std::make_shared<JoinClosure>();
      j->parameters=cmd->parameters;j->definition=cmd->definition;j->env=env;j->joins=scope;j->result=ret;
      scope->joins[cmd->name]=j;
      return eval_cmd(cmd->next,env,scope,ret);
    }
    case Command::Tag::Jump: {
      auto it=joins->joins.find(cmd->name);
      if(it==joins->joins.end())throw std::runtime_error("sequent evaluator: unknown label "+cmd->name);
      auto j=it->second;
      if(j->parameters.size()!=cmd->arguments.size())throw std::runtime_error("sequent evaluator: jump arity mismatch");
      Env scope=child(j->env);
      for(std::size_t i=0;i<cmd->arguments.size();++i)
        scope->values[j->parameters[i].name]=thunk(cmd->arguments[i],env);
      auto lexical_joins=j->joins.lock();
      if(!lexical_joins)throw std::runtime_error("sequent evaluator: expired join scope");
      return eval_cmd(j->definition,scope,lexical_joins,j->result);
    }
    }
    throw std::runtime_error("sequent evaluator: invalid command tag");
  }
public:
  explicit Evaluator(std::size_t max_steps=100000):max_steps_(max_steps){}
  Value run(const Module &m,bool verify=true) {
    if(verify) {
      auto result=Verifier{}.check(m);
      if(!result.ok())throw std::invalid_argument("sequent evaluator: ill-typed module: "+result.errors[0]);
    }
    steps_=0;clear_recursive_caches();
    struct Guard {Evaluator &e;~Guard(){e.clear_recursive_caches();}} cleanup{*this};
    Env env=std::make_shared<Environment>();
    for(const auto &g:m.globals) {
      if(g.name.rfind("$nt.",0)!=0)throw std::invalid_argument("reference evaluator only implements built-in '$nt.*' globals");
      Value v;v.tag=Value::Tag::Native;v.native_name=g.name;
      auto cell=std::make_shared<Cell>();cell->cached=v;
      env->values[g.name]=cell;
    }
    return eval_cmd(m.entry,env,std::make_shared<JumpEnvironment>(),[](Value v){return v;});
  }
  std::size_t steps() const noexcept {return steps_;}
};
} // namespace moonstone::nerotypical::sequent
