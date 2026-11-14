# Moonstone architecture and interfaces

Moonstone has six reusable C++20 subsystems sharing `core` public types. Chisel consumes control-flow and memory events, Supersonic synthesizes pure expressions from finite examples, Syren builds diagnostics from validated S-expressions, ExoML executes a limited declarative command language through host-registered callbacks, Plaid supplies concurrency primitives and a compiled C runtime, and Prolific supplies compiled profiling/cache/Lua libraries. These are **initial realizations**, not a substitute for a mature compiler.

`core/Core.hpp` defines SourceSpan, Diagnostic, ProofResult, SourceEdit, `apply_edits`, and JSON escaping. `core/SExpr.hpp` defines a depth- and node-bounded S-expression parser, syntax trees, variable pattern matching, and substitutions. `core/SafetyEffects.hpp` defines interoperation contracts. Every semantic boundary must distinguish proved/refuted/unknown.

## Chisel

Include `<chisel/Chisel.hpp>`; construct a `Function` containing blocks and `Instruction`s; call `analyze`. Each instruction references a `place`; borrow operations create unique tokens; `EndBorrow` must refer to the matching token. A block successor creates a CFG edge. `analyze` merges states conservatively and emits `CHISEL-*` errors. `converged=false` is an unsafe/inconclusive result and must be rejected by a safety-enforcing host. `Regions.hpp` provides transitive outlives constraints (not full lifetime inference). `GC.hpp` is a demonstration tracing heap only.

## Syren

Create a `Catalog`, insert a parsed rule with `parse_rule`, and call `diagnose` with a matched S-expression **only after the corresponding semantic check**. `StderrSink` prints compact diagnostics; `SarifSink` collects SARIF results. `Repair.hpp` applies non-overlapping byte edits and accepts an externally supplied proof callback. It never claims proof without the callback returning Proven.

## Supersonic

Create `Problem{Grammar,examples,budget,predicate}`, call `solve`; the finite result may be Found, UnsatisfiableWithinBound, BudgetExhausted or InvalidInput. Operations are `Constant`, `Variable`, `Add`, `Sub`, `Mul`, and `Neg` with checked signed arithmetic. The CEGIS helper takes a counterexample oracle. The Lua module lives in `supersonic/lua/lssonic` and can be loaded by adjusting `package.path`.

## ExoML

`Parser` uses generated Aurocks/ASDL code for `NAME ~setconst! Service:Name(args);`,
`~setvar!`, `~get!`, and `~hatch!`, preserving source locations and enforcing input
budgets. `register_service` supplies generic callbacks; `bind_native` deduces
supported C/C++ signatures with checked conversions. `StaticLibrary` installs
selected/renamed exports atomically. Privileged services require `enable_hatch`,
`allow_service` and `~hatch!`; this is not process isolation. `StaticFFI.hpp`
validates declarations and emits compiler-checked bindings, ABI checks/probes
and reflection JSON. `install_static_ffi` exposes an in-memory workspace.
Link `Moonstone::ExoML`; see `exoml/README.md` and `exoml/STATUS.md`.

## Prolific

Include `prolific/Prolific.hpp` and link `Moonstone::Prolific`. Hosts supply compiler,
VM and runtime observations to a bounded, synchronized `Recorder`; `Scope` measures
thread-affine nested regions. `Cache` atomically persists snapshots and JSON using
vendored lmdbxx over LMDB. Link `Moonstone::ProlificLua` to replay events through
native Kaguya-backed Lua `lprofcc`/`lprofvm` callbacks. Analysis findings are
heuristics, not proofs or verified repairs. Linux preload injection observes
`-finstrument-functions` hosts only; arbitrary binary attachment is unsupported.
See `prolific/README.md` for the v1 C ABI, failure/completeness semantics and limits.

`prolific/Speculation.hpp` adds typed observations over existing events and a
bounded speculation pass. Its output is a Lua `lspecir` module containing
evidence profiles, heuristic advice, warnings and unenforced guards. A user
`lspecir.translator` handles those instructions; no compiler lowering is implied.
Incomplete/insufficient/truncated profiles cannot produce speculative candidates.
