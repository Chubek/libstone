# lxbind

`lxbind.lua` is the Lua-side helper module for XBind specs. Specs are ordinary
Lua files, usually named `<library>.xbind`, that return a manifest built from the
helper constructors:

- `xbind.library { ... }`
- `xbind.target("Python", { ... })`
- `xbind.typedef("const char *", "str", { ... })`
- `xbind.param("name", "int", { ... })`
- `xbind.func("symbol", { returns = "int", params = { ... } })`
- `xbind.module("name", { functions = { ... } })`

The helper performs only structural validation. The driver is responsible for
executing specs in a controlled Lua environment, resolving headers, applying type
maps, and rejecting target-specific mistakes.

`qamrpp_xbind.cpp` builds `libqamrpp_xbind.so`, a native QaMRpp module exposing the
same constructor vocabulary for hosts that want the XBind helpers without loading
the Lua source file.
