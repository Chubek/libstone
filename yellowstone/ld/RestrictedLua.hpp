// Private Lua runner utilities for typed build-input hooks, not the layout SDK.
#pragma once

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <cstdlib>
#include <initializer_list>
#include <memory>

namespace qld::detail::restrictedLua {

inline constexpr size_t maxSource = 64 * 1024;
inline constexpr size_t maxBytes = 2 * 1024 * 1024;
inline constexpr int hookInterval = 1000;
inline constexpr int maxInstructions = 1000000;

struct Budget {
  size_t bytes = 0;
  bool allocationLimitHit = false;
  int instructions = 0;
  const char* name;
};

inline void* allocate(void* user, void* ptr, size_t oldSize, size_t newSize) {
  auto& budget = *static_cast<Budget*>(user);
  if (!ptr) oldSize = 0; // For a new block, Lua passes its type instead.
  if (!newSize) {
    std::free(ptr);
    budget.bytes -= oldSize;
    return nullptr;
  }
  const size_t otherBytes = budget.bytes - oldSize;
  if (newSize > maxBytes - otherBytes) {
    budget.allocationLimitHit = true;
    return nullptr;
  }
  if (void* next = std::realloc(ptr, newSize)) {
    budget.bytes = otherBytes + newSize;
    return next;
  }
  return nullptr;
}

inline void countInstructions(lua_State* lua, lua_Debug*) {
  void* user = nullptr;
  lua_getallocf(lua, &user);
  auto& budget = *static_cast<Budget*>(user);
  budget.instructions += hookInterval;
  if (budget.instructions >= maxInstructions)
    luaL_error(lua, "%s instruction limit exceeded", budget.name);
}

using State = std::unique_ptr<lua_State, decltype(&lua_close)>;
inline State create(Budget& budget, unsigned seed) {
  return State(lua_newstate(allocate, &budget, seed), lua_close);
}

// Call from a protected C function. Leaves the new environment on top.
inline void installGlobals(lua_State* lua) {
  luaopen_base(lua);
  lua_newtable(lua);
  for (const char* name : {"assert", "error", "ipairs", "type"}) {
    lua_getfield(lua, -2, name);
    lua_setfield(lua, -2, name);
  }
  lua_remove(lua, -2);
  lua_pushvalue(lua, -1);
  lua_rawseti(lua, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
}

} // namespace qld::detail::restrictedLua
