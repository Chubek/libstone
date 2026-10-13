#pragma once
#include "../blackstone.hpp"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <stdexcept>
#include <deque>

struct blackstone_runtime {
    struct Entry { blackstone::Kernel info; int run = LUA_NOREF; };
    lua_State* state = nullptr;
    std::deque<Entry> entries;
    std::string error;
    blackstone_runtime();
    ~blackstone_runtime();
    void load(const std::string& path);
};

namespace blackstone::lua {
struct StackGuard {
    lua_State* state; int top;
    explicit StackGuard(lua_State* L) : state(L), top(lua_gettop(L)) {}
    ~StackGuard() { lua_settop(state, top); }
};
inline std::string error_text(lua_State* L) {
    const char* text = lua_tostring(L, -1);
    return text ? text : "Lua error (non-string error value)";
}
inline void checked_call(lua_State* L, int args, int results) {
    if (lua_pcall(L, args, results, 0) != LUA_OK) throw std::runtime_error(error_text(L));
}
// Validation must not invoke kernel-defined metamethods outside a protected call.
inline void raw_field(lua_State* L, int index, const char* name) {
    index = lua_absindex(L, index);
    if (!lua_istable(L, index)) throw std::runtime_error("expected table while reading " + std::string(name));
    lua_pushstring(L, name); lua_rawget(L, index);
}
std::string string_field(lua_State* L, int index, const char* name);
std::vector<std::string> string_list(lua_State* L, int index, const char* name);
}
