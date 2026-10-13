#pragma once
#include <string>

struct lua_State;
namespace blackstone::lua {
void load_library(lua_State* state);
void push_ssa_context(lua_State* state, const void* graph, bool modular);
void read_ssa_context(lua_State* state, int index, void* graph);
void register_matching(lua_State* state, int table);
} // namespace blackstone::lua
