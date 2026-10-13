# XBind target compilers

Each subdirectory is the planned lowering stage for one target language. A target
compiler consumes normalized XBind IR and emits target glue that uses the matching
DomBind runtime library.

Current target directories:

- `D.c` -> `dombind/domdbind`
- `Go.c` -> `dombind/domgobind`
- `Java.c` -> `dombind/domjavabind`
- `Lua.c` -> `dombind/domluabind`
- `NodeJS.c` -> `dombind/domnodebind`
- `OCaml.c` -> `dombind/domocamlbind`
- `Python.c` -> `dombind/dompybind`
- `Ruby.c` -> `dombind/domrubybind`
- `Rust.c` -> `dombind/domrustbind`

The first backend should implement a small traceable pipeline:

1. accept an XBind IR dump,
2. resolve target type mappings,
3. request DomBind primitives,
4. emit source files plus a target-native smoke test.

Current status: these directories are placeholders only. The repository specs
are kept to callback-free/query-oriented surfaces so the future IR and emitter
work can start from the same all-target contract instead of per-language one-off
fixtures. The recommended bring-up order is:

1. emit a target-neutral IR dump for `examples/hello/hello.xbind`,
2. lower that IR through `Lua.c` and `domluabind`,
3. add `Dogfooding.xbind` as the second fixture,
4. expand the same IR to the remaining target directories,
5. only then add records, callbacks, buffers, and ownership-transfer lowering.
