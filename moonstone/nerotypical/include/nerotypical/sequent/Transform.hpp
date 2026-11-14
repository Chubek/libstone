#pragma once
#include "Syntax.hpp"
#include <cstddef>
#include <set>
#include <string>
#include <utility>

namespace moonstone::nerotypical::sequent {
// Recursive structures are heap-owned trees, not raw pointers; the following
// transformations construct new trees and never mutate their inputs.
inline TermPtr clone_term(const TermPtr &);
inline CmdPtr clone_cmd(const CmdPtr &);
inline Kont clone_kont(const Kont &k) {
  Kont out;
  for(auto f:k.frames) {
    if(f.argument) f.argument=clone_term(f.argument);
    for(auto &a:f.alternatives) a.body=clone_cmd(a.body);
    out.frames.push_back(std::move(f));
  }
  return out;
}
inline TermPtr clone_term(const TermPtr &v) {
  if(!v) return {};
  auto out=std::make_shared<Term>(*v);
  for(auto &t:out->operands) t=clone_term(t);
  out->command=clone_cmd(v->command);
  return out;
}
inline CmdPtr clone_cmd(const CmdPtr &c) {
  if(!c) return {};
  auto out=std::make_shared<Command>(*c);
  out->term=clone_term(c->term);out->value=clone_term(c->value);
  for(auto &b:out->recursive_values)b.value=clone_term(b.value);
  out->kont=clone_kont(c->kont);
  for(auto &a:out->arguments) a=clone_term(a);
  out->definition=clone_cmd(c->definition);out->next=clone_cmd(c->next);
  return out;
}
namespace detail {
inline void rename_value_cmd(CmdPtr &c,const std::string &from,const std::string &to);
inline void rename_value_term(TermPtr &v,const std::string &from,const std::string &to) {
  if(!v)return;
  if(v->tag==Term::Tag::Var && v->name==from) v->name=to;
  if(v->tag==Term::Tag::Lambda && v->name==from)return;
  for(auto &a:v->operands)rename_value_term(a,from,to);
  rename_value_cmd(v->command,from,to);
}
inline void rename_value_kont(Kont &k,const std::string &from,const std::string &to) {
  for(auto &f:k.frames) {
    rename_value_term(f.argument,from,to);
    for(auto &a:f.alternatives) {
      bool shadow=false;
      for(const auto &b:a.binders) if(b.name==from) shadow=true;
      if(!shadow)rename_value_cmd(a.body,from,to);
    }
  }
}
inline void rename_value_cmd(CmdPtr &c,const std::string &from,const std::string &to) {
  if(!c)return;
  switch(c->tag) {
  case Command::Tag::Cut:rename_value_term(c->term,from,to);rename_value_kont(c->kont,from,to);break;
  case Command::Tag::Jump:for(auto &a:c->arguments)rename_value_term(a,from,to);break;
  case Command::Tag::LetValue:
    rename_value_term(c->value,from,to);
    if(c->name!=from)rename_value_cmd(c->next,from,to);
    break;
  case Command::Tag::LetRec: {
    bool shadow=false;
    for(const auto &b:c->recursive_values)if(b.name==from)shadow=true;
    if(!shadow) {
      for(auto &b:c->recursive_values)rename_value_term(b.value,from,to);
      rename_value_cmd(c->next,from,to);
    }
    break;
  }
  case Command::Tag::LetJoin: {
    bool shadow=false;
    for(const auto &p:c->parameters) if(p.name==from)shadow=true;
    if(!shadow)rename_value_cmd(c->definition,from,to);
    rename_value_cmd(c->next,from,to);
    break;
  }
  }
}
inline bool has_var_term(const TermPtr &t,const std::string &n);
inline bool has_var_cmd(const CmdPtr &c,const std::string &n);
inline bool has_var_kont(const Kont &k,const std::string &n) {
  for(const auto &f:k.frames) {
    if(has_var_term(f.argument,n))return true;
    for(const auto &a:f.alternatives) {
      bool shadow=false;
      for(const auto &b:a.binders) if(b.name==n) shadow=true;
      if(!shadow && has_var_cmd(a.body,n))return true;
    }
  }
  return false;
}
inline bool has_var_term(const TermPtr &t,const std::string &n) {
  if(!t)return false;
  if(t->tag==Term::Tag::Var && t->name==n)return true;
  if(t->tag==Term::Tag::Lambda && t->name==n)return false;
  for(const auto &a:t->operands)if(has_var_term(a,n))return true;
  return has_var_cmd(t->command,n);
}
inline bool has_var_cmd(const CmdPtr &c,const std::string &n) {
  if(!c)return false;
  switch(c->tag) {
  case Command::Tag::Cut:return has_var_term(c->term,n)||has_var_kont(c->kont,n);
  case Command::Tag::Jump:for(auto &a:c->arguments)if(has_var_term(a,n))return true;return false;
  case Command::Tag::LetValue:return has_var_term(c->value,n)||(c->name!=n && has_var_cmd(c->next,n));
  case Command::Tag::LetRec:
    for(const auto &b:c->recursive_values)if(b.name==n)return false;
    for(const auto &b:c->recursive_values)if(has_var_term(b.value,n))return true;
    return has_var_cmd(c->next,n);
  case Command::Tag::LetJoin: {
    bool shadow=false;
    for(const auto &p:c->parameters)if(p.name==n)shadow=true;
    return (!shadow && has_var_cmd(c->definition,n))||has_var_cmd(c->next,n);
  }
  }
  return false;
}
inline bool has_jump(const CmdPtr &c,const std::string &n);
inline bool jump_in_term(const TermPtr &t,const std::string &n) {
  if(!t)return false;
  for(auto &arg:t->operands)if(jump_in_term(arg,n))return true;
  return has_jump(t->command,n);
}
inline bool has_jump(const CmdPtr &c,const std::string &n) {
  if(!c)return false;
  if(c->tag==Command::Tag::Jump) {
    if(c->name==n)return true;
    for(auto &arg:c->arguments)if(jump_in_term(arg,n))return true;
    return false;
  }
  if(c->tag==Command::Tag::Cut) {
    if(jump_in_term(c->term,n))return true;
    for(auto &f:c->kont.frames) {
      if(jump_in_term(f.argument,n))return true;
      for(auto &a:f.alternatives)if(has_jump(a.body,n))return true;
    }
    return false;
  }
  if(c->tag==Command::Tag::LetValue)
    return jump_in_term(c->value,n)||has_jump(c->next,n);
  if(c->tag==Command::Tag::LetRec) {
    for(const auto &b:c->recursive_values)if(jump_in_term(b.value,n))return true;
    return has_jump(c->next,n);
  }
  // The binding shadows the same label in both its definition and body.
  if(c->name==n)return false;
  return has_jump(c->definition,n)||has_jump(c->next,n);
}
// A form of continuation substitution: replace only terminal *ret* uses,
// not returns contained in nested Mu terms (which bind their own ret).
inline CmdPtr plug(const CmdPtr &c,const Kont &suffix) {
  if(!c)return {};
  CmdPtr r=clone_cmd(c);
  switch(r->tag) {
  case Command::Tag::Cut: {
    auto &frames=r->kont.frames;
    if(!frames.empty() && frames.back().tag==Frame::Tag::Case) {
      for(auto &alt:frames.back().alternatives)alt.body=plug(alt.body,suffix);
    } else {
      Kont extra=clone_kont(suffix);
      for(auto &f:extra.frames) frames.push_back(std::move(f));
    }
    break;
  }
  case Command::Tag::Jump:break;
  case Command::Tag::LetValue:r->next=plug(r->next,suffix);break;
  case Command::Tag::LetRec:r->next=plug(r->next,suffix);break;
  case Command::Tag::LetJoin:r->definition=plug(r->definition,suffix);r->next=plug(r->next,suffix);break;
  }
  return r;
}
inline std::set<std::string> &used_names() {
  static thread_local std::set<std::string> used;
  return used;
}
inline void collect_names_cmd(const CmdPtr &c);
inline void collect_names_term(const TermPtr &t) {
  if(!t)return;
  if(!t->name.empty())used_names().insert(t->name);
  for(const auto &a:t->operands)collect_names_term(a);
  collect_names_cmd(t->command);
}
inline void collect_names_cmd(const CmdPtr &c) {
  if(!c)return;
  if(!c->name.empty())used_names().insert(c->name);
  collect_names_term(c->term);
  collect_names_term(c->value);
  for(const auto &b:c->recursive_values) {
    used_names().insert(b.name);
    collect_names_term(b.value);
  }
  for(const auto &p:c->parameters)used_names().insert(p.name);
  for(const auto &a:c->arguments)collect_names_term(a);
  for(const auto &f:c->kont.frames) {
    collect_names_term(f.argument);
    for(const auto &alt:f.alternatives) {
      for(const auto &p:alt.binders)used_names().insert(p.name);
      collect_names_cmd(alt.body);
    }
  }
  collect_names_cmd(c->definition);
  collect_names_cmd(c->next);
}
inline void seed_names(const CmdPtr &c) { used_names().clear(); collect_names_cmd(c); }
inline std::string fresh(const std::string &prefix) {
  static std::size_t next=0;
  std::string proposed;
  do {proposed="__nt_"+prefix+"_"+std::to_string(++next);}while(used_names().count(proposed));
  used_names().insert(proposed);
  return proposed;
}
inline CmdPtr simplify_once(const CmdPtr &c,bool &changed);
inline TermPtr simplify_term(const TermPtr &t,bool &changed) {
  auto r=clone_term(t);
  if(!r)return r;
  for(auto &a:r->operands) a=simplify_term(a,changed);
  if(r->command)r->command=simplify_once(r->command,changed);
  return r;
}
inline CmdPtr simplify_once(const CmdPtr &c,bool &changed) {
  if(!c)return {};
  auto r=clone_cmd(c);
  switch(r->tag) {
  case Command::Tag::LetValue:
    r->value=simplify_term(r->value,changed);r->next=simplify_once(r->next,changed);
    if(!has_var_cmd(r->next,r->name)) {changed=true;return r->next;}
    break;
  case Command::Tag::LetRec:
    for(auto &b:r->recursive_values)b.value=simplify_term(b.value,changed);
    r->next=simplify_once(r->next,changed);break;
  case Command::Tag::LetJoin:
    r->definition=simplify_once(r->definition,changed);
    r->next=simplify_once(r->next,changed);
    if(!has_jump(r->next,r->name)) {changed=true;return r->next;}
    break;
  case Command::Tag::Jump:
    for(auto &a:r->arguments) {a=simplify_term(a,changed);}
    break;
  case Command::Tag::Cut:
    r->term=simplify_term(r->term,changed);
    for(auto &f:r->kont.frames) {
      if(f.argument)f.argument=simplify_term(f.argument,changed);
      for(auto &a:f.alternatives)a.body=simplify_once(a.body,changed);
    }
    if(r->term->tag==Term::Tag::Mu) {
      changed=true;return plug(r->term->command,r->kont);
    }
    if(!r->kont.frames.empty()) {
      const Frame f=r->kont.frames.front();
      if(f.tag==Frame::Tag::Apply && r->term->tag==Term::Tag::Lambda) {
        // Use a fresh binder and a shared let rather than duplicating the
        // argument: this preserves call-by-name/need semantics.
        const auto old=r->term->name;
        const auto fresh_name=fresh("arg");
        TermPtr body=clone_term(r->term->operands.at(0));
        rename_value_term(body,old,fresh_name);
        Kont rest;rest.frames.assign(r->kont.frames.begin()+1,r->kont.frames.end());
        changed=true;
        return make::let_value(fresh_name,r->term->type.args.at(0),clone_term(f.argument),make::cut(body,rest));
      }
      if(f.tag==Frame::Tag::Case && r->kont.frames.size()==1) {
        bool match=false;
        Alternative chosen;
        if(r->term->tag==Term::Tag::Bool) {
          for(const auto &a:f.alternatives)if((r->term->boolean && a.tag==Alternative::Tag::True)||(!r->term->boolean && a.tag==Alternative::Tag::False)||a.tag==Alternative::Tag::Default) {chosen=a;match=true;break;}
        } else if(r->term->tag==Term::Tag::Constructor) {
          for(const auto &a:f.alternatives)if((a.tag==Alternative::Tag::Constructor && a.constructor==r->term->name)||a.tag==Alternative::Tag::Default) {chosen=a;match=true;break;}
        }
        if(match) {
          CmdPtr body=clone_cmd(chosen.body);
          if(chosen.tag==Alternative::Tag::Default) body=make::let_value(chosen.binders.at(0).name,r->term->type,clone_term(r->term),body);
          else if(chosen.tag==Alternative::Tag::Constructor) {
            for(std::size_t i=chosen.binders.size();i>0;--i) {
              const auto &b=chosen.binders[i-1];
              body=make::let_value(b.name,b.type,clone_term(r->term->operands.at(i-1)),body);
            }
          }
          changed=true;return body;
        }
      }
    }
    break;
  }
  return r;
}
// Only truly saturated tail calls in the current return scope qualify.
// A lambda closure, captured function, or call inside nested Mu disqualifies.
inline bool tail_only(const CmdPtr &c,const std::string &fn,std::size_t arity,int &calls) {
  if(!c)return true;
  switch(c->tag) {
  case Command::Tag::Cut: {
    if(c->term && c->term->tag==Term::Tag::Var && c->term->name==fn) {
      if(c->kont.frames.size()!=arity)return false;
      for(const auto &f:c->kont.frames)if(f.tag!=Frame::Tag::Apply || has_var_term(f.argument,fn))return false;
      ++calls;return true;
    }
    return !has_var_term(c->term,fn) && [&](){
      for(const auto &f:c->kont.frames) {
        if(has_var_term(f.argument,fn))return false;
        for(const auto &a:f.alternatives)if(!tail_only(a.body,fn,arity,calls))return false;
      }
      return true;
    }();
  }
  case Command::Tag::Jump:
    for(const auto &a:c->arguments)if(has_var_term(a,fn))return false;
    return true;
  case Command::Tag::LetValue:
    if(has_var_term(c->value,fn))return false;
    return c->name==fn || tail_only(c->next,fn,arity,calls);
  case Command::Tag::LetRec:
    for(const auto &b:c->recursive_values)if(b.name==fn)return true;
    for(const auto &b:c->recursive_values)if(has_var_term(b.value,fn))return false;
    return tail_only(c->next,fn,arity,calls);
  case Command::Tag::LetJoin:
    for(const auto &p:c->parameters)if(p.name==fn)return tail_only(c->next,fn,arity,calls);
    return tail_only(c->definition,fn,arity,calls)&&tail_only(c->next,fn,arity,calls);
  }
  return false;
}
inline CmdPtr change_calls(const CmdPtr &c,const std::string &f,const std::string &j) {
  if(!c)return {};
  auto r=clone_cmd(c);
  switch(r->tag) {
  case Command::Tag::Cut:
    if(r->term && r->term->tag==Term::Tag::Var && r->term->name==f) {
      std::vector<TermPtr> args;
      for(auto &frame:r->kont.frames)args.push_back(frame.argument);
      return make::jump(j,std::move(args));
    }
    for(auto &frame:r->kont.frames)for(auto &alt:frame.alternatives)alt.body=change_calls(alt.body,f,j);
    break;
  case Command::Tag::Jump:break;
  case Command::Tag::LetValue:if(r->name!=f)r->next=change_calls(r->next,f,j);break;
  case Command::Tag::LetRec: {
    bool shadows=false;
    for(const auto &b:r->recursive_values)if(b.name==f)shadows=true;
    if(!shadows)r->next=change_calls(r->next,f,j);
    break;
  }
  case Command::Tag::LetJoin:
    r->definition=change_calls(r->definition,f,j);
    r->next=change_calls(r->next,f,j);break;
  }
  return r;
}
inline TermPtr contify_term(const TermPtr &t,bool &changed);
inline CmdPtr contify_once(const CmdPtr &c,const Type &ret,bool &changed) {
  if(!c)return {};
  auto r=clone_cmd(c);
  switch(r->tag) {
  case Command::Tag::Cut:
    r->term=contify_term(r->term,changed);
    for(auto &f:r->kont.frames) {
      f.argument=contify_term(f.argument,changed);
      for(auto &a:f.alternatives)a.body=contify_once(a.body,ret,changed);
    }
    break;
  case Command::Tag::Jump:
    for(auto &a:r->arguments)a=contify_term(a,changed);
    break;
  case Command::Tag::LetJoin:
    r->definition=contify_once(r->definition,ret,changed);
    r->next=contify_once(r->next,ret,changed);
    break;
  case Command::Tag::LetRec:
    for(auto &b:r->recursive_values)b.value=contify_term(b.value,changed);
    r->next=contify_once(r->next,ret,changed);
    break;
  case Command::Tag::LetValue: {
    r->value=contify_term(r->value,changed);
    r->next=contify_once(r->next,ret,changed);
    std::vector<Parameter> args;
    auto v=r->value;
    while(v && v->tag==Term::Tag::Lambda && v->type.tag==Type::Tag::Arrow) {
      args.push_back({v->name,v->type.args.at(0)});
      v=v->operands.at(0);
    }
    if(args.empty()||!v||v->tag!=Term::Tag::Mu || v->type!=ret)return r;
    int calls=0;
    { Type t=v->type;for(auto it=args.rbegin();it!=args.rend();++it)t=Type::arrow(it->type,t);if(t!=r->bound_type)return r; }
    // This restriction preserves the current return continuation exactly.
    if(!tail_only(r->next,r->name,args.size(),calls) || calls==0)return r;
    if(has_var_cmd(v->command,r->name))return r;
    const auto j=fresh("join");
    changed=true;
    return make::let_join(j,args,clone_cmd(v->command),change_calls(r->next,r->name,j));
  }
  }
  return r;
}
inline TermPtr contify_term(const TermPtr &t,bool &changed) {
  if(!t)return {};
  auto r=clone_term(t);
  for(auto &a:r->operands)a=contify_term(a,changed);
  if(r->tag==Term::Tag::Mu)r->command=contify_once(r->command,r->type,changed);
  return r;
}
}
inline CmdPtr append_continuation(const CmdPtr &c,const Kont &suffix) {return detail::plug(c,suffix);}
struct RewriteStats {std::size_t passes=0;bool reached_limit=false;bool changed=false;};
inline CmdPtr simplify(const CmdPtr &c,RewriteStats *statistics=nullptr,std::size_t max_passes=64) {
  detail::seed_names(c);
  auto result=clone_cmd(c); RewriteStats stats;
  for(std::size_t i=0;i<max_passes;++i) {
    bool changed=false;
    auto next=detail::simplify_once(result,changed);
    if(!changed){if(statistics)*statistics=stats;return result;}
    result=std::move(next);stats.changed=true;++stats.passes;
  }
  stats.reached_limit=true;if(statistics)*statistics=stats;return result;
}
inline CmdPtr contify(const CmdPtr &c,const Type &return_type,bool *changed_out=nullptr) {
  detail::seed_names(c);
  bool changed=false;auto result=detail::contify_once(c,return_type,changed);
  if(changed_out)*changed_out=changed;
  return result;
}
inline Module optimize(Module m,RewriteStats *stats=nullptr) {
  if(!m.entry)return m;
  m.entry=contify(m.entry,m.result_type);
  m.entry=simplify(m.entry,stats);
  return m;
}
} // namespace moonstone::nerotypical::sequent
