# Dependency integration

GateML adds no dependencies and performs no network fetches. Its source inputs
are the checkout's existing vendored implementations:

| Component | Inputs | Role |
|---|---|---|
| libchomsky3 | `third_party/libchomsky3/src`, `include` | Regex compiler and bytecode matcher |
| libglr | `third_party/libglr/src/glr`, `include` | GLR tables, parser, forest, rewrites |
| NovoParse | `third_party/novoparse/source`, `profiles`, `runtime` | GateML parser generator and labeled-tree adapter |
| Jiterati | `jiterati/include/Jiterati.hpp` | Semantic IR construction |
| klib/ctl | `third_party/klib`, `third_party/ctl` | libglr runtime containers and allocation |
| sfsexp | `third_party/libglr/third_party/sfsexp` | libglr rewrite/query support |

`cmake/GLR.cmake` builds the top-level libglr runtime with the populated
libstone klib/ctl copies because its nested klib/ctl submodules are empty in
this checkout. Database cache support is disabled; sfsexp and threading remain
linked. An existing `libglr` target can be reused by an embedding build.

`cmake/Chomsky.cmake` builds libchomsky3's bytecode path without the two SLJIT
sources, which upstream includes even when regex JIT is disabled. No SLJIT
headers or binaries are needed. An existing `chomsky3` target can be reused.

NovoParse's D CLI compiles the checked-in GateML grammar to a generated C++
header in the build directory. Its native runtime is compiled separately so
that it links to the requested top-level libglr, avoiding a second runtime
from NovoParse's vendored copy. Generation depends on both frontend sources
and built-in profiles. Generated artifacts never enter the source tree.

Jiterati's core is header-only, so GateML does not build unrelated Jiterati
CLIs, machine backends, Lua bindings, or documentation. IR modules produced
by GateML can subsequently be compiled by applications linking those backends.
The empty GateML D scaffold is preserved while the native bootstrap API is C++.

Each dependency retains its existing license. GateML's dependency adapters
change build composition only, not third-party source. The native baseline
uses libglr's POSIX facilities and is tested on Linux.
