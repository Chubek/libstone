# Standard-library implementation status

Implemented, embedded, typechecked and tested:

* `Prelude.gml`: combinators and integer helpers.
* `Int.gml`, `Bool.gml`, `Pair.gml`: scalar/pair operations.
* `Option.gml`, `Result.gml`: typed success/absence/error combinators.
* `String.gml`: byte-string operations.
* `List.gml`: immutable lists and higher-order traversal.
* `IO.gml`: captured textual output.

Typed host primitives are defined in `../Prelude.cpp` and implemented in
`../FFI.cpp`. Other functions are written in GateML, not disconnected C++ stubs.
`stdlib.cpp` exposes the generated embedded source. `stdlib.gml` documents its
assembly order; it is not an import loader.

The pre-existing **empty directories** below are future work, not implemented
modules, and are deliberately not installed:

`Array`, `Map`, `Stack`, `Deque`, `Graph`, `CTypes`, `Parsec`, `JSON`, `YAML`,
`BSON`, `MSGPack`, `SExpr`, `Shell`, `Reflect`, `AST`, `Disasm`, `Assembler`,
`Target`, `Toolchain`, `IPC`, `Gatestone`, `Limestone`, `Yellowstone`,
`Blackstone`, `Sandstone`, `Firestone`, `Tourchstone`, `Strict`, `Package`,
`Archive`, `IR`, `Dump`, `ELF`, `MachO`, `PE`, `COFF`, `Logger`, `Printer`,
`Formatter`, `Optimizer`, `LALR`, `GLR`, `Pipeline`, `GC`, `FFI`, `HTTP`, `FTP`,
`Git`, and `FUSE`.

The same-named empty directories `Prelude/`, `String/`, `List/`, and `IO/` are
unused scaffolding; the implemented code is in the corresponding `.gml` files.
This release does **not** complete every planned standard-library facility.
See [the language manual](../README.md) for exact signatures, semantics and
backend limitations.
