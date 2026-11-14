#pragma once
// Optional integration with Nerotypical's original Satie-backed solvers.
// Include only in builds that already provide <Satie.hpp>/<SatieEUF.hpp>.
#include "Syntax.hpp"
#include "../SetTyping.hpp"
#include "../StructuralTyping.hpp"
#include <optional>
#include <stdexcept>

namespace moonstone::nerotypical::sequent {
inline Type from_structural(const structural::StructuralType &s) {
  if(s.kind==structural::StructuralType::Kind::Rec)
    throw std::invalid_argument("recursive structural types require explicit equi-/iso-recursive extension");
  if(s.kind==structural::StructuralType::Kind::Variable)return Type::var(s.name);
  if(s.name=="int" && s.args.empty())return Type::integer();
  if(s.name=="float" && s.args.empty())return Type::floating();
  if(s.name=="string" && s.args.empty())return Type::string();
  if(s.name=="bool" && s.args.empty())return Type::boolean();
  if(s.name=="unit" && s.args.empty())return Type::unit();
  if(s.name=="fun" && s.args.size()==2)
    return Type::arrow(from_structural(s.args[0]),from_structural(s.args[1]));
  std::vector<Type> args;
  for(const auto &arg:s.args)args.push_back(from_structural(arg));
  return Type::data(s.name,std::move(args));
}
// Set-typing witnesses are sets of possible atoms, not function types.
// Conversion is only meaningful for a uniquely resolved ground atom.
inline std::optional<Type> singleton_atom(const settype::TypeUniverse &universe,
                                          const settype::AtomSet &atoms) {
  if(atoms.size()!=1)return std::nullopt;
  const auto &name=universe.name(atoms.front());
  if(name=="int")return Type::integer();
  if(name=="float")return Type::floating();
  if(name=="string")return Type::string();
  if(name=="bool")return Type::boolean();
  if(name=="unit")return Type::unit();
  return Type::data(name);
}
inline std::optional<Type> from_inferred(const structural::StructuralResult &result,
                                         structural::TermId type_variable) {
  if(result.base.status!=AnalysisStatus::WellTyped)return std::nullopt;
  for(const auto &[id,type]:result.inferred)
    if(id==type_variable)return from_structural(type);
  return std::nullopt;
}
inline std::optional<Type> from_set_witness(const settype::SetTypingResult &result,
                                            const settype::TypeUniverse &universe,
                                            settype::TypeVarId variable) {
  if(result.base.status!=AnalysisStatus::WellTyped || variable>=result.witness.size())
    return std::nullopt;
  // This is a satisfying witness, not necessarily a principal/must type.
  return singleton_atom(universe,result.witness[variable]);
}
} // namespace moonstone::nerotypical::sequent
