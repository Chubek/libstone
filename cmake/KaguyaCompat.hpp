#pragma once
extern "C" {
#include <lua.h>
}
// Kaguya's two legacy GC tuning helpers predate Lua 5.5's parameter API.
// Adapt their calls, rather than assigning the old opcodes new meanings.
#if LUA_VERSION_NUM >= 505
#define LUA_GCSETPAUSE (-1001)
#define LUA_GCSETSTEPMUL (-1002)
inline int libstone_kaguya_gc(lua_State *state, int operation, int value) {
    if (operation == LUA_GCSETPAUSE) return lua_gc(state, LUA_GCPARAM, LUA_GCPPAUSE, value);
    if (operation == LUA_GCSETSTEPMUL) return lua_gc(state, LUA_GCPARAM, LUA_GCPSTEPMUL, value);
    return lua_gc(state, operation, value);
}
#define lua_gc libstone_kaguya_gc
#endif
#include <kaguya/kaguya.hpp>
#if LUA_VERSION_NUM >= 505
#undef lua_gc
#undef LUA_GCSETPAUSE
#undef LUA_GCSETSTEPMUL
#endif
