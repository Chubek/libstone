#include <cstdio>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
int luaopen_flintstone(lua_State*);
}
int main() {
    auto* state = luaL_newstate();
    if (!state) return 2;
    luaL_openlibs(state);
    luaL_requiref(state, "flintstone", luaopen_flintstone, 1);
    lua_pop(state, 1);
    const auto status = luaL_dostring(state, R"LUA(
        local f = require('flintstone')
        assert(f.flintstone_version() == '0.1.0')
        assert(f.flintstone_binding_target_count() == 9)
        local context = assert(f.flintstone_context_create())
        local module = assert(f.flintstone_parse_text(context, [[
          module lua level high {func @f(%x:i32)->i32 {
            ^e: %r:i32=math.square(%x); func.return(%r);
          }}
        ]]))
        assert(f.flintstone_verify(context, module) == 0)
        assert(f.flintstone_lower(context, module, 1) == 0)
        assert(f.flintstone_module_text(module):find('arith.mul', 1, true))
        local imp = assert(f.flintstone_parse_text(context, [[
          module m level core { import "math"; func @f(%x:i32)->i32 {
            ^e: %r:i32=func.call(%x) attrs {callee="std.math.abs_i32";}; func.return(%r);
          }}
        ]]))
        assert(f.flintstone_verify(context, imp) ~= 0)
        assert(f.flintstone_resolve_imports(context, imp, nil) == 0)
        assert(f.flintstone_verify(context, imp) == 0)
        assert(f.flintstone_module_text(imp):find('std.math.abs_i32', 1, true))
        f.flintstone_module_destroy(imp)
        assert(f.flintstone_parse_text(context, 'invalid') == nil)
        assert(#f.flintstone_last_error() > 0)
        assert(not pcall(f.flintstone_parse_text, context, 'a'..string.char(0)..'b'))
        f.flintstone_module_destroy(module)
        f.flintstone_context_destroy(context)
    )LUA");
    if (status) std::fprintf(stderr, "%s\n", lua_tostring(state, -1));
    lua_close(state); return status ? 1 : 0;
}
