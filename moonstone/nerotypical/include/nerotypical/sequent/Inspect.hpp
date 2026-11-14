#pragma once
#include "Syntax.hpp"
#include "Verify.hpp"
#include "Transform.hpp"
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace moonstone::nerotypical::sequent {
struct Metrics {
  std::size_t terms=0,commands=0,join_bindings=0,jumps=0,
              cuts=0,mu_abstractions=0,case_alternatives=0,
              max_continuation_frames=0;
};
namespace inspect_detail {
inline void measure_command(const CmdPtr &,Metrics &);
inline void measure_term(const TermPtr &t,Metrics &m) {
  if(!t)return;
  ++m.terms;
  if(t->tag==Term::Tag::Mu)++m.mu_abstractions;
  for(const auto &a:t->operands)measure_term(a,m);
  measure_command(t->command,m);
}
inline void measure_command(const CmdPtr &c,Metrics &m) {
  if(!c)return;
  ++m.commands;
  switch(c->tag) {
  case Command::Tag::Cut:
    ++m.cuts;measure_term(c->term,m);
    if(c->kont.frames.size()>m.max_continuation_frames)m.max_continuation_frames=c->kont.frames.size();
    for(auto &f:c->kont.frames) {
      measure_term(f.argument,m);
      m.case_alternatives+=f.alternatives.size();
      for(const auto &a:f.alternatives)measure_command(a.body,m);
    }
    break;
  case Command::Tag::Jump:
    ++m.jumps;
    for(const auto &a:c->arguments)measure_term(a,m);
    break;
  case Command::Tag::LetValue:
    measure_term(c->value,m);measure_command(c->next,m);break;
  case Command::Tag::LetRec:
    for(const auto &b:c->recursive_values)measure_term(b.value,m);
    measure_command(c->next,m);break;
  case Command::Tag::LetJoin:
    ++m.join_bindings;measure_command(c->definition,m);measure_command(c->next,m);break;
  }
}
inline std::string escape_dot(std::string s) {
  std::string r;
  for(char c:s) {if(c=='"'||c=='\\')r.push_back('\\');if(c=='\n'||c=='\r')r.push_back(' ');else r.push_back(c);}
  return r;
}
struct DotBuilder {
  std::ostringstream dot;
  int next=0;
  int node(std::string label) {int n=next++;dot<<"  n"<<n<<" [label=\""<<escape_dot(label)<<"\"];\n";return n;}
  void edge(int a,int b,std::string label="") {dot<<"  n"<<a<<" -> n"<<b;if(!label.empty())dot<<" [label=\""<<escape_dot(label)<<"\"]";dot<<";\n";}
  int visit(const CmdPtr &c) {
    if(!c)return node("NULL");
    switch(c->tag) {
    case Command::Tag::Cut: {
      int n=node("cut ("+std::to_string(c->kont.frames.size())+" frames)");
      for(const auto &f:c->kont.frames) {
        if(f.tag==Frame::Tag::Case) {
          int decision=node("case");edge(n,decision);
          for(const auto &a:f.alternatives) {
            std::string pattern=a.tag==Alternative::Tag::Constructor?a.constructor:(a.tag==Alternative::Tag::True?"True":a.tag==Alternative::Tag::False?"False":"_");
            edge(decision,visit(a.body),pattern);
          }
        }
      }
      return n;
    }
    case Command::Tag::Jump:return node("jump "+c->name);
    case Command::Tag::LetValue: {
      int n=node("let "+c->name);edge(n,visit(c->next));return n;
    }
    case Command::Tag::LetRec: {
      int n=node("let-rec ("+std::to_string(c->recursive_values.size())+")");
      edge(n,visit(c->next));return n;
    }
    case Command::Tag::LetJoin: {
      int n=node("join "+c->name);
      edge(n,visit(c->definition),"definition");
      edge(n,visit(c->next),"body");return n;
    }
    }
    return node("unknown");
  }
};
}
inline Metrics inspect(const Module &m) {Metrics r;inspect_detail::measure_command(m.entry,r);return r;}
inline std::string to_dot(const Module &m) {
  inspect_detail::DotBuilder b;b.dot<<"digraph SequentIR {\n  rankdir=TB;\n  node [shape=box];\n";
  b.visit(m.entry);b.dot<<"}\n";return b.dot.str();
}
// Verification gate for the public optimizer.  On failure, no partially
// transformed module is returned; diagnostics remain inspectable via exception.
inline Module optimize_checked(const Module &input,RewriteStats *stats=nullptr) {
  const auto before=Verifier{}.check(input);
  if(!before.ok())throw std::invalid_argument("input IR rejected: "+before.errors.front());
  auto after=optimize(input,stats);
  const auto check=Verifier{}.check(after);
  if(!check.ok())throw std::logic_error("optimizer violated IR typing/scoping: "+check.errors.front());
  return after;
}
} // namespace moonstone::nerotypical::sequent
