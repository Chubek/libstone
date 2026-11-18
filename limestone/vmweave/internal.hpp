#pragma once
#include "vmweave.hpp"
#include <sstream>
#include <iomanip>
namespace limestone::vmweave {
inline Result<Module> checked_module(const Module& m) {
  auto s=m.configuration; s.instructions.clear();
  for(const auto& w:m.words) s.instructions.push_back({w.name,w.c_body,w.effect,w.flow,w.opcode,w.operands,w.source,w.abstract});
  return lower(std::move(s));
}
struct AbstractProfile {
  size_t inputs = 0, outputs = 0;
  std::string_view flow = "next";
};
// Returns a diagnostic or the inferred entry/exit profile.
Result<AbstractProfile> profile_abstract(const Specification&, const Instruction&);
std::string print_abstract(const AbstractProgram&);
std::string lower_abstract_c(const Specification&, const Word&);
inline std::pair<size_t,size_t> effect_counts(const std::string& effect) {
  std::istringstream in(effect); std::string token; size_t before=0,after=0; bool output=false;
  while(in>>token) {if(token=="--") output=true; else if(token!="(" && token!=")") (output?after:before)++;}
  return {before,after};
}
inline unsigned stk_version(const Module& m) {
  return std::any_of(m.words.begin(),m.words.end(),[](const auto& w){return w.abstract.has_value();})?2:1;
}
inline std::string c_string(std::string_view s) {
  std::ostringstream out; out<<'"';
  for(unsigned char c:s) {
    if(c=='"' || c=='\\') out<<'\\'<<static_cast<char>(c);
    else if(c<32 || c>=127) out<<'\\'<<std::oct<<std::setw(3)<<std::setfill('0')<<unsigned(c)<<std::dec;
    else out<<static_cast<char>(c);
  }
  out<<'"'; return out.str();
}
}
