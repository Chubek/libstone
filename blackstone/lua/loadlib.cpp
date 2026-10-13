#include "runtime.hpp"
#include "../lrewrite/lrewrite.hpp"
#include "lrewrite_source.hpp"

namespace blackstone::lua {
void load_library(lua_State* L) {
    luaL_openlibs(L);
    if (luaL_loadbufferx(L, lrewrite_source, sizeof(lrewrite_source) - 1,
                         "@blackstone/lrewrite.lua", "t") != LUA_OK)
        throw std::runtime_error(error_text(L));
    checked_call(L, 0, 1);
    if (luaL_loadbufferx(L, local_rewrite_source, sizeof(local_rewrite_source) - 1,
                         "@blackstone/lrewrite/local.lua", "t") != LUA_OK)
        throw std::runtime_error(error_text(L));
    checked_call(L, 0, 1);
    lua_setfield(L, -2, "local_rewrite");
    register_matching(L, -1);
    lua_setglobal(L, "lrewrite");
}
}

blackstone_runtime::blackstone_runtime() {
    state = luaL_newstate();
    if (!state) throw std::bad_alloc();
    try { blackstone::lua::load_library(state); }
    catch (...) { lua_close(state); state = nullptr; throw; }
}
blackstone_runtime::~blackstone_runtime() { if (state) lua_close(state); }
