#pragma once
#include "Syntax.hpp"
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace moonstone::nerotypical::sequent {
struct Verification {
  std::vector<std::string> errors;
  bool ok() const noexcept {return errors.empty();}
};
class Verifier {
  using Values=std::map<std::string,Type>;
  using TypeVars=std::set<std::string>;
  struct Join {std::vector<std::string> tparams; std::vector<Parameter> params;};
  using Joins=std::map<std::string,Join>;
  const Module *module_=nullptr;
  Verification output_;
  void fail(const std::string &where,const std::string &s) {output_.errors.push_back(where+": "+s);}
  bool valid_type(const Type &t,const TypeVars &vs,const std::string &where) {
    bool good=true;
    if(t.tag==Type::Tag::Var && !vs.count(t.name)) {fail(where,"unbound type variable '"+t.name+"'");good=false;}
    if(t.tag==Type::Tag::Arrow && t.args.size()!=2) {fail(where,"arrow expects two types");return false;}
    if(t.tag==Type::Tag::Forall && t.args.size()!=1) {fail(where,"forall expects one body");return false;}
    TypeVars inner=vs;
    if(t.tag==Type::Tag::Forall) {if(t.name.empty() || vs.count(t.name)) {fail(where,"invalid forall binder");good=false;}inner.insert(t.name);}
    for(const auto &a:t.args) if(!valid_type(a,inner,where)) good=false;
    return good;
  }
  Type check_term(const TermPtr &t,const Values &v,const TypeVars &tv,const std::string &where) {
    if(!t) {fail(where,"null term");return Type::unit();}
    const auto annotated=t->type;
    valid_type(annotated,tv,where);
    Type inferred=annotated;
    switch(t->tag) {
    case Term::Tag::Var: {
      auto it=v.find(t->name);
      if(it==v.end()) fail(where,"unbound value '"+t->name+"'");
      else inferred=it->second;
      break;
    }
    case Term::Tag::Int: inferred=Type::integer();break;
    case Term::Tag::Float: inferred=Type::floating();break;
    case Term::Tag::String: inferred=Type::string();break;
    case Term::Tag::Bool: inferred=Type::boolean();break;
    case Term::Tag::Unit: inferred=Type::unit();break;
    case Term::Tag::Lambda: {
      if(annotated.tag!=Type::Tag::Arrow || annotated.args.size()!=2 || t->operands.size()!=1 || t->name.empty()) {fail(where,"malformed lambda");break;}
      Values scope=v;scope[t->name]=annotated.args[0];
      Type res=check_term(t->operands[0],scope,tv,where+"/lambda");
      if(res!=annotated.args[1]) fail(where,"lambda result mismatch: "+type_string(res)+" vs "+type_string(annotated.args[1]));
      inferred=Type::arrow(annotated.args[0],res);break;
    }
    case Term::Tag::TypeLambda: {
      if(annotated.tag!=Type::Tag::Forall || annotated.args.size()!=1 || annotated.name!=t->name || t->operands.size()!=1 || t->name.empty() || tv.count(t->name)) {fail(where,"malformed type lambda");break;}
      auto scope=tv;scope.insert(t->name);
      Type res=check_term(t->operands[0],v,scope,where+"/type-lambda");
      inferred=Type::forall(t->name,res);break;
    }
    case Term::Tag::Constructor: {
      const DataConstructor *ctor=nullptr;
      for(const auto &c:module_->constructors) if(c.name==t->name) ctor=&c;
      if(!ctor) {fail(where,"unknown data constructor '"+t->name+"'");break;}
      if(t->operands.size()!=ctor->fields.size()) fail(where,"constructor arity mismatch for '"+t->name+"'");
      for(std::size_t i=0;i<t->operands.size() && i<ctor->fields.size();++i) {
        auto actual=check_term(t->operands[i],v,tv,where+"/field");
        if(actual!=ctor->fields[i]) fail(where,"constructor field "+std::to_string(i)+" type mismatch");
      }
      inferred=ctor->result;break;
    }
    case Term::Tag::Mu: {
      // A term is not allowed to capture any ambient labels.  A fresh
      // continuation namespace enforces that property by construction.
      check_cmd(t->command,v,{},tv,annotated,where+"/mu");break;
    }
    }
    if(inferred!=annotated) fail(where,"annotation "+type_string(annotated)+" differs from inferred "+type_string(inferred));
    return inferred;
  }
  void check_kont(const Kont &k,Type input,const Values &v,const Joins &joins,const TypeVars &tv,const Type &ret,const std::string &where) {
    bool saw_case=false;
    for(std::size_t i=0;i<k.frames.size();++i) {
      const auto &f=k.frames[i];
      if(saw_case) {fail(where,"frame after case continuation");return;}
      switch(f.tag) {
      case Frame::Tag::Apply: {
        const auto actual=check_term(f.argument,v,tv,where+"/arg");
        if(input.tag!=Type::Tag::Arrow || input.args.size()!=2) {fail(where,"attempted application of non-function "+type_string(input));return;}
        if(input.args[0]!=actual) fail(where,"argument type mismatch: "+type_string(actual)+" vs "+type_string(input.args[0]));
        input=input.args[1];break;
      }
      case Frame::Tag::TypeApply: {
        valid_type(f.type_argument,tv,where);
        if(input.tag!=Type::Tag::Forall || input.args.size()!=1) {fail(where,"type instantiation of non-polymorphic term");return;}
        try {input=substitute_type(input.args[0],input.name,f.type_argument);}
        catch(const std::exception &e) {fail(where,e.what());return;}
        break;
      }
      case Frame::Tag::Case: {
        saw_case=true;
        if(i+1!=k.frames.size()) fail(where,"case must terminate continuation");
        if(f.alternatives.empty()) fail(where,"empty case");
        std::set<std::string> seen;
        bool has_default=false,truth=false,falsity=false;
        for(const auto &alt:f.alternatives) {
          Values scope=v;
          if(!alt.body) {fail(where,"null case branch");continue;}
          if(alt.tag==Alternative::Tag::Default) {
            if(has_default || alt.binders.size()!=1 || alt.binders[0].type!=input) fail(where,"invalid default alternative");
            has_default=true;
          } else if(alt.tag==Alternative::Tag::True || alt.tag==Alternative::Tag::False) {
            if(input!=Type::boolean() || !alt.binders.empty()) fail(where,"Boolean pattern on non-Bool or with binders");
            if(alt.tag==Alternative::Tag::True) truth=true;else falsity=true;
          } else {
            auto it=module_->constructors.begin();
            for(;it!=module_->constructors.end();++it) if(it->name==alt.constructor) break;
            if(it==module_->constructors.end() || it->result!=input) fail(where,"constructor pattern mismatches scrutinee");
            else {
              if(it->fields.size()!=alt.binders.size()) fail(where,"pattern binder arity mismatch");
              for(std::size_t a=0;a<it->fields.size() && a<alt.binders.size();++a)
                if(it->fields[a]!=alt.binders[a].type) fail(where,"pattern binder type mismatch");
            }
          }
          std::string key=alt.tag==Alternative::Tag::Constructor?alt.constructor:std::to_string(static_cast<int>(alt.tag));
          if(!seen.insert(key).second) fail(where,"duplicate case alternative " +key);
          for(const auto &param:alt.binders) {
            if(param.name.empty()) fail(where,"empty pattern binder");
            scope[param.name]=param.type;
          }
          check_cmd(alt.body,scope,joins,tv,ret,where+"/alt");
        }
        if(!has_default) {
          if(input==Type::boolean() && (!truth || !falsity)) fail(where,"non-exhaustive Boolean case");
          if(input.tag==Type::Tag::Data) {
            for(const auto &ctor:module_->constructors) if(ctor.result==input && !seen.count(ctor.name)) fail(where,"non-exhaustive data case: missing "+ctor.name);
          }
          if(input.tag!=Type::Tag::Data && input!=Type::boolean()) fail(where,"non-exhaustive case on unenumerated type");
        }
        return;
      }
      }
    }
    if(input!=ret) fail(where,"continuation returns "+type_string(input)+" instead of "+type_string(ret));
  }
  void check_cmd(const CmdPtr &c,const Values &v,const Joins &joins,const TypeVars &tv,const Type &ret,const std::string &where) {
    if(!c) {fail(where,"null command");return;}
    switch(c->tag) {
    case Command::Tag::Cut: {
      Type t=check_term(c->term,v,tv,where+"/term");
      check_kont(c->kont,t,v,joins,tv,ret,where+"/kont");break;
    }
    case Command::Tag::Jump: {
      auto it=joins.find(c->name);
      if(it==joins.end()) {fail(where,"unbound jump label '"+c->name+"'");break;}
      const auto &j=it->second;
      if(c->type_arguments.size()!=j.tparams.size() || c->arguments.size()!=j.params.size()) fail(where,"jump arity mismatch for '"+c->name+"'");
      for(std::size_t i=0;i<c->arguments.size() && i<j.params.size();++i) {
        Type want=j.params[i].type;
        for(std::size_t k=0;k<c->type_arguments.size() && k<j.tparams.size();++k)
          try {want=substitute_type(want,j.tparams[k],c->type_arguments[k]);}catch(const std::exception &e){fail(where,e.what());}
        if(check_term(c->arguments[i],v,tv,where+"/jump-arg")!=want) fail(where,"jump argument "+std::to_string(i)+" type mismatch");
      }
      break;
    }
    case Command::Tag::LetValue: {
      valid_type(c->bound_type,tv,where);
      if(check_term(c->value,v,tv,where+"/value")!=c->bound_type) fail(where,"let value type mismatch");
      Values scope=v;scope[c->name]=c->bound_type;
      check_cmd(c->next,scope,joins,tv,ret,where+"/let");break;
    }
    case Command::Tag::LetRec: {
      Values scope=v;
      for(const auto &bind:c->recursive_values) {
        if(bind.name.empty() || scope.count(bind.name))fail(where,"duplicate recursive binding '"+bind.name+"'");
        valid_type(bind.type,tv,where+"/rec-type");
        scope[bind.name]=bind.type;
      }
      for(const auto &bind:c->recursive_values)
        if(check_term(bind.value,scope,tv,where+"/rec-value")!=bind.type)
          fail(where,"recursive initializer type mismatch for '"+bind.name+"'");
      check_cmd(c->next,scope,joins,tv,ret,where+"/rec-body");break;
    }
    case Command::Tag::LetJoin: {
      Joins scope=joins;
      if(joins.count(c->name)) fail(where,"join label shadows existing join '"+c->name+"'");
      scope[c->name]={c->type_parameters,c->parameters};
      Values local=v;TypeVars local_t=tv;
      for(const auto &t:c->type_parameters) {
        if(t.empty() || !local_t.insert(t).second) fail(where,"duplicate type parameter on join");
      }
      std::set<std::string> local_params;
      for(const auto &p:c->parameters) {
        valid_type(p.type,local_t,where);
        if(p.name.empty() || !local_params.insert(p.name).second) fail(where,"duplicate join parameter '"+p.name+"'");
        local[p.name]=p.type;
      }
      check_cmd(c->definition,local,scope,local_t,ret,where+"/join-definition");
      check_cmd(c->next,v,scope,tv,ret,where+"/join-body");break;
    }
    }
  }
public:
  Verification check(const Module &m) {
    module_=&m;output_={};Values v;TypeVars tv;
    for(const auto &p:m.globals) {if(v.count(p.name)) fail("module","duplicate global: "+p.name);v[p.name]=p.type;valid_type(p.type,tv,"module/global");}
    std::set<std::string> ctor_names;
    for(const auto &c:m.constructors) {
      if(!ctor_names.insert(c.name).second) fail("module","duplicate constructor: "+c.name);
      valid_type(c.result,tv,"module/constructor");
      for(const auto &t:c.fields) valid_type(t,tv,"module/field");
    }
    valid_type(m.result_type,tv,"module/return");
    check_cmd(m.entry,v,{},tv,m.result_type,"entry");return output_;
  }
};
} // namespace moonstone::nerotypical::sequent
