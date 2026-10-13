# XBind

XBind is the Tourchstone binding generator scaffold. It is intended to play the
same role as SWIG for this tree: read a C API, apply a Lua-written binding spec,
normalize the requested surface, map C types into a target language, then lower
the result through the DomBind runtime libraries.

This directory is currently an initial vertical scaffold, not a complete C parser
or binding generator. The committed pieces establish the project shape, target
inventory, Lua spec vocabulary, default type-map format, examples, repository
spec inventory, and checks that future compiler stages can grow into. See
[`MANUAL.md`](MANUAL.md) for the spec authoring guide and current binding
coverage.

## Directory map

| Path | Purpose |
| --- | --- |
| `xbind.c`, `xbind.h`, `xbind.hpp` | Small C/C++ API plus a scaffold CLI for discovery and layout checks. |
| `xbind_qamrpp.*` | QaMRpp-backed executor for `.xbind` Lua specs. |
| `MANUAL.md` | Spec authoring guide, target matrix, current repository specs, and limitations. |
| `cmake/XBindQaMRpp.cmake` | CMake locator for the configured `third_party/qamrpp` checkout (`XBIND_QAMRPP_ROOT`). |
| `lxbind/` | Lua helpers used by `.xbind`/`.lua` specs. |
| `typedefs/` | Default per-target C type maps, loaded before user overrides. |
| `stdlib/` | Planned reusable spec/runtime helpers. |
| `compilers/<Target>.c/` | Per-target compiler/lowering stages from XBind IR to DomBind. |
| `dombind/` | Native target runtime/boundary libraries used by generated bindings. |
| `examples/` | Executable examples and spec fixtures. |
| `Dogfooding.xbind` | XBind's self-binding spec for the stable `xbind.h` query surface. |
| `XFeats.schema.json` | Feature-manifest schema for binding transformation features. |

## Supported target scaffold

| Target | DomBind library | Default type map |
| --- | --- | --- |
| D | `dombind/domdbind` | `typedefs/D-Default.sexpr` |
| Go | `dombind/domgobind` | `typedefs/Go-Default.sexpr` |
| Java | `dombind/domjavabind` | `typedefs/Java-Default.sexpr` |
| Lua | `dombind/domluabind` | `typedefs/Lua-Default.sexpr` |
| NodeJS | `dombind/domnodebind` | `typedefs/NodeJs-Default.sexpr` |
| OCaml | `dombind/domocamlbind` | `typedefs/OCaml-Default.sexpr` |
| Python | `dombind/dompybind` | `typedefs/Python-Default.sexpr` |
| Ruby | `dombind/domrubybind` | `typedefs/Ruby-Default.sexpr` |
| Rust | `dombind/domrustbind` | `typedefs/Rust-Default.sexpr` |

## Spec files

Specs are Lua files with either `.xbind` or `.lua` extensions. By convention a
library named `hello` is described by `hello.xbind`, falling back to `hello.lua`.
The spec returns a manifest table built with `lxbind` helpers:

```lua
local xbind = require("lxbind")

return xbind.library {
  name = "hello",
  headers = { "hello.h" },
  targets = {
    xbind.target("D", { module = "hello" }),
    xbind.target("Go", { package = "hello" }),
    xbind.target("Java", { package = "stone.hello", class = "Hello" }),
    xbind.target("Lua", { module = "hello" }),
    xbind.target("NodeJS", { addon = "hello" }),
    xbind.target("OCaml", { library = "hello" }),
    xbind.target("Python", { module = "hello" }),
    xbind.target("Ruby", { extension = "hello" }),
    xbind.target("Rust", { crate = "hello_sys" }),
  },
  functions = {
    xbind.func("hello_add", {
      returns = "int",
      params = { xbind.param("left", "int"), xbind.param("right", "int") },
    }),
  },
}
```

User specs may also add type overrides. XBind loads the relevant
`typedefs/<Target>-Default.sexpr` first, then applies spec-local entries and any
requested standard-library helpers.

Repository specs currently target all supported backends. The first implemented
surface is deliberately callback-free and record-free so every backend can share
the same lowering contract while header parsing and IR generation are brought up.
See `MANUAL.md` for the per-spec coverage table.

## Build and smoke checks

Standalone build:

```sh
cmake -S tourchstone/xbind -B /tmp/opencode/xbind-build -DXBIND_BUILD_TESTS=ON
cmake --build /tmp/opencode/xbind-build
ctest --test-dir /tmp/opencode/xbind-build --output-on-failure
```

Useful CLI probes:

```sh
/tmp/opencode/xbind-build/xbind --list-targets
/tmp/opencode/xbind-build/xbind --check-layout tourchstone/xbind
/tmp/opencode/xbind-build/xbind --find-spec hello tourchstone/xbind/examples/hello
/tmp/opencode/xbind-build/xbind --dump-spec hello tourchstone/xbind/examples/hello
```

The `--check-spec` and `--dump-spec` commands execute the spec with the
configured QaMRpp Lua-compatible interpreter (`XBIND_QAMRPP_ROOT`, normally
`third_party/qamrpp` or `${LIBSTONE_THIRD_PARTY_DIR}/qamrpp`). The build also
emits `libqamrpp_xbind.so`, a QaMRpp native module that exposes the same
XBind/LxBind constructor surface to QaMRpp hosts.

DomBind remains opt-in because each runtime may require a different SDK:

```sh
cmake -S tourchstone/xbind -B /tmp/opencode/xbind-build \
  -DXBIND_BUILD_DOMBIND=ON -DDOMBIND_LUA=ON
```

## Implementation pipeline

1. Resolve the spec (`<library>.xbind`, then `<library>.lua`) and execute it with
   `lxbind` on the Lua path through QaMRpp.
2. Parse the referenced C headers and collect exported declarations.
3. Merge default target type maps with user overrides.
4. Build a normalized XBind IR: modules, functions, records, enums, ownership,
   errors, callbacks, and enabled XFeats.
5. Lower XBind IR into the selected `compilers/<Target>.c` backend.
6. Emit target glue using the matching `dombind/dom<target>bind` runtime.
7. Compile and run generated smoke tests for each requested target.

See [`REGIMEN.md`](REGIMEN.md) for the staged validation plan.
