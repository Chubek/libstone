#include "match.hpp"
#include "../lrewrite/lrewrite.hpp"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace blackstone {
std::optional<Substitution> match(const Pattern& pattern, const Term& term) {
    return firestone::rewrite::match(pattern, term);
}
namespace lua {
static int lua_match(lua_State* L) {
    const char* pattern = luaL_checkstring(L, 1);
    const char* term = luaL_checkstring(L, 2);
    // Exit the catch before raising a Lua error (longjmp).
    bool failed = false;
    try {
        auto result = blackstone::match(parse_pattern(pattern), firestone::rewrite::parse(term));
        if (!result) { lua_pushnil(L); return 1; }
        lua_newtable(L);
        for (const auto& binding : *result) {
            auto text = firestone::rewrite::to_string(binding.second);
            lua_pushlstring(L, text.data(), text.size());
            lua_setfield(L, -2, binding.first.c_str());
        }
    } catch (const std::exception& e) { lua_pushstring(L, e.what()); failed = true; }
    if (failed) return lua_error(L);
    return 1;
}
void register_matching(lua_State* L, int table) {
    table = lua_absindex(L, table);
    lua_pushcfunction(L, lua_match); lua_setfield(L, table, "match");
}
}
}
