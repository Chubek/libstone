# Moonstone architecture and interfaces

Moonstone is four independently buildable C++20 INTERFACE-library components sharing `core` public types. Chisel consumes control-flow and memory events, Supersonic synthesizes pure expressions from finite examples, Syren builds diagnostics from validated S-expressions, and ExoML executes a limited declarative command language through host-registered callbacks. These are **initial realizations**, not a substitute for a mature compiler.

`core/Core.hpp` defines SourceSpan, Diagnostic, ProofResult, SourceEdit, `apply_edits`, and JSON escaping. `core/SExpr.hpp` defines a depth- and node-bounded S-expression parser, syntax trees, variable pattern matching, and substitutions. `core/SafetyEffects.hpp` defines interoperation contracts. Every semantic boundary must distinguish proved/refuted/unknown.

## Chisel

Include `<chisel/Chisel.hpp>`; construct a `Function` containing blocks and `Instruction`s; call `analyze`. Each instruction references a `place`; borrow operations create unique tokens; `EndBorrow` must refer to the matching token. A block successor creates a CFG edge. `analyze` merges states conservatively and emits `CHISEL-*` errors. `converged=false` is an unsafe/inconclusive result and must be rejected by a safety-enforcing host. `Regions.hpp` provides transitive outlives constraints (not full lifetime inference). `GC.hpp` is a demonstration tracing heap only.

## Syren

Create a `Catalog`, insert a parsed rule with `parse_rule`, and call `diagnose` with a matched S-expression **only after the corresponding semantic check**. `StderrSink` prints compact diagnostics; `SarifSink` collects SARIF results. `Repair.hpp` applies non-overlapping byte edits and accepts an externally supplied proof callback. It never claims proof without the callback returning Proven.

## Supersonic

Create `Problem{Grammar,examples,budget,predicate}`, call `solve`; the finite result may be Found, UnsatisfiableWithinBound, BudgetExhausted or InvalidInput. Operations are `Constant`, `Variable`, `Add`, `Sub`, `Mul`, and `Neg` with checked signed arithmetic. The CEGIS helper takes a counterexample oracle. The Lua module lives in `supersonic/lua/lssonic` and can be loaded by adjusting `package.path`.

## ExoML

`Parser` recognizes `NAME ~setconst! Service:Name(args);`, `~setvar!`, `~get!`, and `~hatch!`. `Interpreter::register_service` supplies callbacks. `enable_hatch` plus `allow_service` and `~hatch!` are required for privileged services. This explicitly limits accidental calls but does not isolate native code in a process sandbox. The separate `StaticFFI.hpp` provides a declaration model and validation only.
