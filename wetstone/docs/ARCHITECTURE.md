# Wetstone architecture

## Layers

- `utils/IR.hpp` is a minimal analysis-facing representation of SSA IDs, CFG blocks, operands, operations and successors. It isolates domain implementations from direct dependencies on execution backends. `ir::Function::validate()` checks block identity, duplicate SSA definitions, terminator positioning and Phi predecessor coverage. `utils/SSAValidation.hpp` additionally verifies dominance for reachable SSA uses.
- `abstract-interp/` supplies finite and infinite abstract domains, join, meet, widening, transfer and a forward analysis driver. The abstract domain contract is in `AbstractDomain.hpp`.
- `dataflow-analysis/` implements direction-parametric worklist propagation and classic lattice analyses.
- `path-analysis/` provides CFG graph queries, bounded path enumeration and path-sensitive abstract execution.
- `pointer-analysis/` represents a flow-insensitive inclusion-constraint system, with conservative unknown objects and separate reporting utilities.
- `symbolic-exec/` owns typed term trees, path conditions, SMT-LIB2, a Boolean Tseitin transformation and optional Satie SAT/LIA services.
- `repair-suggest/` reports candidates; it does not mutate program code. It can use Satie for Boolean candidate selection.
- `interprocedural/` constructs call graphs and propagates finite effect summaries across recursion.
- `verification/` implements proof obligations, assertion checks and arithmetic hazard detection.
- `integration/` maps upstream IRs into Wetstone's IR and enables Satie/Equinox-NG when reachable.

## Semantics

`wetstone::ir::Opcode` includes arithmetic, control-flow, allocation, memory, calls, assertions and taint sources. Operands are either signed 64-bit immediate constants or SSA value IDs; arbitrary-precision, floating-point and typed pointer values require a specialized adapter/domain.

A function contains uniquely identified blocks. A branch lists its true successor first. Phi values have an incoming block ID for every operand. `utils/Graph.hpp` derives graph structure from each block's final terminator. Analyses should not mutate the IR while using an analysis result.

`Wetstone.hpp` is a convenience umbrella. Smaller clients should import individual headers to reduce compilation cost.

## Worklist invariants

The generic solver initializes all reachable states to bottom, joins predecessor (or successor) facts and applies transfer. For cyclic graphs, delayed widening avoids infinite ascending chains. State updates trigger dependent blocks. An exhausted evaluation budget sets `converged=false`.

`ai::analyze_intervals` returns an `Environment` per block at entry and exit. Missing value bindings mean unconstrained Top, not zero. `Environment::bottom()` denotes unreachable control-flow.

## Solver failure conventions

`symbolic::CheckResult` is a three-valued enum: `Sat`, `Unsat`, `Unknown`. Unknown includes absent dependencies, unsupported Boolean encodings, or genuine inconclusiveness. `verification::discharge` reports `Proved` only when negating the conclusion is **unsatisfiable**.

## Extension points

- Implement a domain with `top()`, `bottom()`, `join`, `meet`, `widen`, `leq`, and equality.
- Pass it to `df::solve<State>(function, transfer, boundary, direction)`.
- Register analyses through `AnalysisPipeline` and use `AnalysisManager` for versioned cached results.
- Supply a conversion from a custom source language to Wetstone's IR, preserving SSA IDs, block IDs, Phi predecessor edges, literal types and integer semantics.
- Provide a `symbolic::Solver` subclass for external SMT backends while preserving `Unknown` where appropriate.
