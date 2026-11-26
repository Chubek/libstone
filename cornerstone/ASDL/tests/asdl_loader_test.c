#define _POSIX_C_SOURCE 200809L
#include "../runtime/asdl_loader.h"

#include <stdio.h>
#include <stdlib.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

static int mark(lua_State *L)
{
    int *count = lua_touserdata(L, lua_upvalueindex(1));
    lua_pushinteger(L, ++*count);
    return 1;
}

static int isnode(lua_State *L)
{
    lua_pushboolean(L, 0);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s TEST_LUA FIXTURE_DIR STDLIB_ROOT\n", argv[0]);
        return 1;
    }
    lua_State *L = luaL_newstate();
    if (!L)
        return 1;
    int count = 0, result = 1;
    luaL_openlibs(L);
    const char *paths = lua_pushfstring(L, ":%s/first::%s/second:", argv[2], argv[2]);
    if (setenv("LIBSTONE_ASDL_PATH", paths, 1) != 0) {
        perror("setenv");
        goto done;
    }
    lua_settop(L, 0);
    lua_newtable(L);
    lua_pushlightuserdata(L, &count);
    lua_pushcclosure(L, mark, 1);
    lua_setfield(L, -2, "mark");
    lua_pushcfunction(L, isnode);
    lua_setfield(L, -2, "isnode");
    lua_newtable(L);
    lua_pushliteral(L, "original");
    lua_setfield(L, -2, "sentinel");
    lua_setfield(L, -2, "diagnostic");
    lua_pushboolean(L, 1); /* Ensure install handles a non-top ASDL table. */
    if (asdl_loader_install(L, -2, argv[3]) != 0 || lua_gettop(L) != 2)
        goto done;
    lua_pop(L, 1);
    lua_setglobal(L, "asdl");
    if (luaL_loadfilex(L, argv[1], "t") != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        goto done;
    }
    lua_pushstring(L, argv[2]);
    lua_pushstring(L, argv[3]);
    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
        goto done;
    }
    result = 0;
done:
    lua_close(L);
    return result;
}
