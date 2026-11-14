#pragma once
#include "Syntax.hpp"
#include <sstream>

namespace moonstone::nerotypical::sequent {
inline std::string print_cmd(const CmdPtr &c,int indent=0);
inline std::string print_term(const TermPtr &t) {
  if(!t)return "<null>";
  switch(t->tag) {
  case Term::Tag::Var:return t->name;
  case Term::Tag::Int:return std::to_string(t->integer);
  case Term::Tag::Float:return std::to_string(t->floating);
  case Term::Tag::String:return "\""+t->text+"\"";
  case Term::Tag::Bool:return t->boolean?"True":"False";
  case Term::Tag::Unit:return "()";
  case Term::Tag::Lambda:return "(lambda "+t->name+":"+type_string(t->type.args.at(0))+". "+print_term(t->operands.at(0))+")";
  case Term::Tag::TypeLambda:return "(Lambda "+t->name+". "+print_term(t->operands.at(0))+")";
  case Term::Tag::Constructor: {
    std::string s=t->name+"{";
    for(std::size_t i=0;i<t->operands.size();++i) {if(i)s+=", ";s+=print_term(t->operands[i]);}
    return s+"}";
  }
  case Term::Tag::Mu:return "(mu ret. "+print_cmd(t->command)+")";
  }
  return "<?>";
}
inline std::string print_kont(const Kont &k,int indent=0) {
  std::string s;
  for(const auto &f:k.frames) {
    switch(f.tag) {
    case Frame::Tag::Apply:s+=" . "+print_term(f.argument);break;
    case Frame::Tag::TypeApply:s+=" . ["+type_string(f.type_argument)+"]";break;
    case Frame::Tag::Case:
      s+=" . case {";
      for(const auto &a:f.alternatives) {
        s+="\n"+std::string(indent+2,' ');
        if(a.tag==Alternative::Tag::True)s+="True";
        else if(a.tag==Alternative::Tag::False)s+="False";
        else if(a.tag==Alternative::Tag::Default)s+="_";
        else s+=a.constructor;
        s+="(";
        for(std::size_t i=0;i<a.binders.size();++i) {if(i)s+=", ";s+=a.binders[i].name+":"+type_string(a.binders[i].type);}
        s+=") -> "+print_cmd(a.body,indent+2);
      }
      s+="\n"+std::string(indent,' ')+"}";break;
    }
  }
  if(k.frames.empty()||k.frames.back().tag!=Frame::Tag::Case)s+=" . ret";
  return s;
}
inline std::string print_cmd(const CmdPtr &c,int indent) {
  if(!c)return "<null>";
  const std::string pad(indent,' ');
  switch(c->tag) {
  case Command::Tag::Cut:return "<"+print_term(c->term)+" ||"+print_kont(c->kont,indent)+">";
  case Command::Tag::Jump: {
    std::string s="jump "+c->name;
    for(auto t:c->type_arguments)s+=" ["+type_string(t)+"]";
    for(auto a:c->arguments)s+=" "+print_term(a);
    return s;
  }
  case Command::Tag::LetValue:return "let "+c->name+":"+type_string(c->bound_type)+" = "+print_term(c->value)+"\n"+pad+"in "+print_cmd(c->next,indent);
  case Command::Tag::LetRec: {
    std::string s="let rec {";
    for(const auto &b:c->recursive_values)
      s+="\n"+std::string(indent+2,' ')+b.name+":"+type_string(b.type)+" = "+print_term(b.value);
    return s+"\n"+pad+"} in "+print_cmd(c->next,indent);
  }
  case Command::Tag::LetJoin: {
    std::string s="join "+c->name;
    for(const auto &t:c->type_parameters)s+=" ["+t+"]";
    for(const auto &p:c->parameters)s+=" ("+p.name+":"+type_string(p.type)+")";
    return s+" =\n"+std::string(indent+2,' ')+print_cmd(c->definition,indent+2)+"\n"+pad+"in "+print_cmd(c->next,indent);
  }
  }
  return "<?>";
}
inline std::string print_module(const Module &m) {return "module : "+type_string(m.result_type)+" =\n  "+print_cmd(m.entry,2)+"\n";}
} // namespace moonstone::nerotypical::sequent
