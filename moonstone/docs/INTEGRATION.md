# Integration contract and planned vendored connections

## Actual inspected vendor paths

- `third_party/memtkx/include/memtkx/MemTKX.hpp`: memory spaces, collection plans, barriers, scheduler.
- `third_party/memtkx-c`: additional C-specific memory toolkit; ABI must be inspected before binding.
- `third_party/satie/include/Satie.hpp`, `SatieCDCL.hpp`, `SatieLIA.hpp`, `SatieSMT.hpp`: solver integration candidates.
- `third_party/equinox-ng/EquinoxNG.hpp`: e-graph/extraction integration candidate.
- `third_party/exolangtk/include/ExtensionTk/etk_api.h`: versioned extension service tables.
- `third_party/exolangtk/include/FFItk/`, `InteropTk/`: foreign-calling and ABI building blocks.
- `third_party/cparser-iso11`, `third_party/ucpp`: ExoML StaticFFI's eventual C frontend.
- `third_party/pkgconf`, `third_party/replxx`: ExoML package resolution and interactive REPL.
- `third_party/lua`, `third_party/kaguya`: native Supersonic Lua bindings.
- `wetstone/utils/IR.hpp`, `wetstone/pointer-analysis/Ownership.hpp`, `wetstone/utils/SARIF.hpp`: compiler analysis and diagnostic foundations.
- `firestone/librewrite/Term.hpp`, `firestone/libeqsat/EGraph.hpp`: pattern and rewrite infrastructure.

## Root CMake integration

Root `CMakeLists.txt` now registers Wetstone and Moonstone through
`LIBSTONE_BUILD_WETSTONE` and `LIBSTONE_BUILD_MOONSTONE` (both default ON).
Moonstone remains independently buildable. The old snippet is historical only;
do not apply it again. Moonstone inherits root test/tool defaults and installs
under the `Moonstone` install component without requiring unrelated targets.

Wetstone's source-tree vendor include paths are now build-interface-only so
CMake can generate its export alongside Moonstone. This does not install vendor
dependencies or certify Wetstone's optional adapters. Full root validation is
still blocked: see `STATUS.md` for the actual failures and tested configurations.

Moonstone's `MOONSTONE_USE_MEMTKX`, `MOONSTONE_USE_SATIE` and
`MOONSTONE_USE_EQUINOX` options deliberately fail with an unsupported-adapter
diagnostic. They must not be restored to presence-only include switches. A
future enabling patch must implement the adapter and add linked executable tests
against the inspected revision. No vendor revision pin/integration is claimed by
this core-only increment.

## Boundary rules

1. Always map upstream language IR into a semantically complete Chisel IR. A lost `Free`, borrow or unknown side effect must not be lowered to a no-op.
2. Never treat solver UNKNOWN as UNSAT; keep time/memory/budget status explicit.
3. Never enable GC without a provably complete root enumeration and correct barriers at all mutator writes.
4. FFI calls require explicit ownership, lifetime, ABI, callback and exception boundaries.
5. Diagnostic patterns select wording and candidate fixes; semantic analyses must prove the violation.
6. Re-run type, ownership and semantic checks on any suggested repair, independent of the generator.
7. Generated code and extension ABI interfaces must be fuzzed and validated against target toolchain ABI.
