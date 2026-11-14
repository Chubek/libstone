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
- `third_party/lua`, `third_party/kaguya`, `third_party/lmdbxx`: actual compiled
  Prolific integrations (requires the LMDB C library/header). Supersonic's proposed
  native bridge remains a separate workstream.
- `wetstone/utils/IR.hpp`, `wetstone/pointer-analysis/Ownership.hpp`, `wetstone/utils/SARIF.hpp`: compiler analysis and diagnostic foundations.
- `firestone/librewrite/Term.hpp`, `firestone/libeqsat/EGraph.hpp`: pattern and rewrite infrastructure.

## Root CMake integration

Use `cmake/ROOT-INTEGRATION.cmake.txt` as a suggested patch fragment for root `CMakeLists.txt`. Moonstone can be built standalone without applying it. Existing project components are untouched.

## Boundary rules

1. Always map upstream language IR into a semantically complete Chisel IR. A lost `Free`, borrow or unknown side effect must not be lowered to a no-op.
2. Never treat solver UNKNOWN as UNSAT; keep time/memory/budget status explicit.
3. Never enable GC without a provably complete root enumeration and correct barriers at all mutator writes.
4. FFI calls require explicit ownership, lifetime, ABI, callback and exception boundaries.
5. Diagnostic patterns select wording and candidate fixes; semantic analyses must prove the violation.
6. Re-run type, ownership and semantic checks on any suggested repair, independent of the generator.
7. Generated code and extension ABI interfaces must be fuzzed and validated against target toolchain ABI.


## Plaid integration (new)

Firestone already supplies `libcps/CPSBuilder.hpp`, `CPSLowering.hpp`,
`Continuation.hpp`, plus `libconcurrency/{Channel,Fiber,GreenThread,Thread}.hpp`,
`libcompute/Compute.hpp` and `libintrin/Vector.hpp`. Plaid's reference
continuation machine is not a replacement for Firestone's typed CPS SSA
lowering; write adapters and differential tests before promising compiler
integration.

Plaid C++ library is `Moonstone::Plaid` and compiled cooperative C runtime
is `Moonstone::PlaidRuntime`. Root integration is opt-in; the ZIP contains
only `moonstone/`, not modified LibStone root sources. Ensure
`LIBSTONE_BUILD_MOONSTONE` is selected and OS threads are available.

Most importantly: the present channel select supports buffered channels and
recv-only cases; one-shot continuations are callbacks, not actual call/cc;
and `weaver` identifies spill slots but does not generate native async frames.
Read `plaid/AGENTS.md` before extending these semantics.

## ExoML static integration

`Moonstone::ExoML` links the compiled interpreter, generated Aurocks/ASDL
frontend, typed StaticFFI wrappers and declaration services. The generators
run offline from `scripts/`; their helper symbols are prefixed to coexist with
other generated parsers. Native calls use actual compiler function types.
Generated-code tests link a C11 fixture and independently compile ABI probes
and intentional signature/layout mismatches. This path does not depend on
ExolangTk's unimplemented `ffi_call`, infer C layouts, or parse C headers.
See `exoml/README.md` and `exoml/STATUS.md` for exact contracts and open adapters.

## Prolific integration

`MOONSTONE_BUILD_PROLIFIC` enables `Moonstone::Prolific` and
`Moonstone::ProlificLua` in standalone and root builds. Generic C/C++ hooks accept
host-owned semantic names and original source/IR identities; adapters must not
claim complete coverage without instrumenting relevant host operations. No
Flintstone/Wetstone-specific adapter is implied by the generic event API. Linux
preloading supports GCC/Clang function-instrumented hosts; uninstrumented binaries
remain unsupported. Lua replay runs outside producer locks on the caller's thread,
not on Plaid workers implicitly. Lua scripts are trusted process code; limited
libraries/instruction hooks do not establish isolation. Cache failures, collection
loss and script failures remain distinct in the API/report. See
`prolific/DESIGN.md` and `prolific/README.md`.

Structured `observe`/`prolific_observe` events preserve the v1 cache format.
`Speculation.hpp` aggregates type/branch/value/call/interoperation/system/cost/
lowering and other explicit host observations into declarative Lua SpecIR.
`lspecir.translator` dispatches validated instructions to user handlers. The
library emits guard requirements, not installed checks or semantic proofs;
target legality, boundary ABI and effect obligations remain with the translator.
See `prolific/SPECIR.md` for the schema and evidence-quality policy.
