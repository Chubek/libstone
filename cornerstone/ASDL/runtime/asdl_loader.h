#ifndef ASDL_LOADER_H
#define ASDL_LOADER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_State lua_State;

/* Install asdl.lib and asdl.signlib on the host table at asdl_index.
 * Open the Lua base, math, string, table and UTF-8 libraries first.
 * NULL stdlib_root uses LIBSTONE_ASDL_STDLIB_DIR if configured at build time.
 * Returns zero and preserves the stack; raises a Lua error on misuse. */
int asdl_loader_install(lua_State *L, int asdl_index, const char *stdlib_root);

#ifdef __cplusplus
}
#endif

#endif
