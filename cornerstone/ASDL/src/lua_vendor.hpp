#pragma once

// Force-include when compiling the vendored Lua sources as C++. Lua uses
// exceptions instead of longjmp in that build, while the public API still
// has C linkage for Kaguya and C extensions. Define Lua's platform settings
// before including any public headers.
#include <lprefix.h>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
