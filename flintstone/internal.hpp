#pragma once
#include "Flintstone.hpp"

namespace flintstone::detail {
[[noreturn]] inline void fail(std::string code, std::string message, Location location = {}) {
    throw Error(std::move(code), std::move(message), std::move(location));
}
inline void check(bool condition, std::string code, std::string message, Location location = {}) {
    if (!condition) fail(std::move(code), std::move(message), std::move(location));
}
std::string dialect_name(const std::string&);
Type resolve(const Type&, const Module&);
Module specialize(const Module&);
struct CFG {
    firestone::ir::ControlFlowGraph graph;
    std::map<BlockId, firestone::ir::BlockId> blocks;
    std::map<firestone::ir::BlockId, BlockId> original;
};
CFG cfg(const Function&, const Registry&);
firestone::ir::Type firestone_type(const Type&);
Type from_firestone_type(const firestone::ir::Type&);
Function from_ssa(const firestone::ir::SimpleSSAGraph&, const Function&);
void install_builtins(Registry&);
void install_vector_dialect(Registry&);
void install_tuple_dialect(Registry&);
void install_ptr_dialect(Registry&);
std::string attribute_text(const Attribute&);
bool valid_name(std::string_view);
}
