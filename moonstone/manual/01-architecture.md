# Moonstone architecture and interfaces

Moonstone is four independently selectable C++20 INTERFACE-library components sharing a compiled `Moonstone::Core` library (static or shared). Chisel consumes control-flow and memory events, Supersonic synthesizes pure expressions from finite examples, Syren builds diagnostics from validated S-expressions, and ExoML executes a limited declarative command language through host-registered callbacks. These are **initial realizations**, not a substitute for a mature compiler.

`core/Core.hpp` defines SourceSpan, Diagnostic, ProofResult, SourceEdit, `apply_edits`, and JSON escaping. `core/SExpr.hpp` defines a depth- and node-bounded S-expression parser, syntax trees, variable pattern matching, and substitutions. `core/SafetyEffects.hpp` defines interoperation contracts. Every semantic boundary must distinguish proved/refuted/unknown.

Core services live in the inline `moonstone::v1` namespace. `Result.hpp`,
`SourceManager.hpp` and `Diagnostics.hpp` supply structured errors, immutable
UTF-8 sources, manager-local symbols and bounded diagnostic retention.
`moonstone.h` is a versioned C source-management ABI. See `03-core-v1.md` for
coordinate units, storage/threading limits, failure behavior and ABI guarantees.

## Chisel

Include `<chisel/Chisel.hpp>`; construct a `Function` containing blocks and `Instruction`s; call `analyze`. Each instruction references a `place`; borrow operations create unique tokens; `EndBorrow` must refer to the matching token. A block successor creates a CFG edge. `analyze` merges states conservatively and emits `CHISEL-*` errors from final reachable inputs. Check `status`, `converged`, diagnostics and `failure_reason`. Invalid CFGs/operations return InvalidInput rather than throwing from `analyze`; exhaustion returns BudgetExhausted. Proven means acceptance by this small explicit-loan policy **only**, not memory safety. `Regions.hpp` provides transitive outlives constraints (not full lifetime inference). `GC.hpp` is a demonstration tracing heap only.

## Syren

Create a `Catalog`, insert a parsed rule with `parse_rule`, and call `diagnose` with a matched S-expression **only after the corresponding semantic check**. `StderrSink` prints compact diagnostics; `SarifSink` collects SARIF results. `Repair.hpp` applies non-overlapping byte edits and accepts an externally supplied proof callback. It never claims proof without the callback returning Proven.

## Supersonic

Create `Problem{Grammar,examples,budget,predicate}`, call `solve`; the finite result may be Found, UnsatisfiableWithinBound, BudgetExhausted or InvalidInput. Operations are `Constant`, `Variable`, `Add`, `Sub`, `Mul`, and `Neg` with portable checked signed arithmetic. Grammar variables must be unique ASCII identifiers and supplied in every example; operations must be supported. Evaluator failure includes overflow, malformed arity, missing variables and resource limits (default depth 256/nodes 100000). It is not a proof. Cost bounds remain 1..12 and candidate counts never exceed their budget.

`cegis_verified` takes a `VerificationOracle` returning `ProofResult` plus an
optional actual counterexample. Only explicit Proven without a counterexample
sets `fully_verified`; proof assumptions remain attached. The host must justify
the oracle independently of synthesis. Refuted requires a new, actually failing
counterexample. Unknown/exhaustion stops without verification. The legacy
optional-counterexample `cegis` is source-compatible but is always provisional
when no counterexample is returned; its former implicit proof behavior is
intentionally removed. The Lua module remains a separate engine in
`supersonic/lua/lssonic`.

## ExoML

`Parser` recognizes `NAME ~setconst! Service:Name(args);`, `~setvar!`, `~get!`, and `~hatch!`. `Interpreter::register_service` supplies callbacks. `enable_hatch` plus `allow_service` and `~hatch!` are required for privileged services. This explicitly limits accidental calls but does not isolate native code in a process sandbox. The separate `StaticFFI.hpp` provides a declaration model and validation only.
