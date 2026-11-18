/* Execute differential regression suite with vendored Lua, without a system CLI. */
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 2) {std::fprintf(stderr,"usage: %s test_extensions.lua\n",argv[0]);return 2;}
    lua_State* L=luaL_newstate();
    if (!L) return 2;
    luaL_openlibs(L);
    const int result=luaL_dofile(L,argv[1]);
    if (result!=LUA_OK) std::fprintf(stderr,"%s\n",lua_tostring(L,-1));
    lua_close(L);
    return result==LUA_OK ? 0:1;
}
