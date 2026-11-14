# Nerotypical: Sequent IR extension

**Source home:** `Chubek/libstone/moonstone/nerotypical`.
**Upstream pin:** `321bcc989ece9e08cc2ba318c21aa14d02d98a3c` (2026-11-14).
**Standard:** C++20. **Status:** executable, tested experimental IR / reference semantics; **not** a completed optimizing code-generator toolchain.

## Relationship to existing Nerotypical

At the pinned revision, Nerotypical already has:

- `Analysis.hpp`: analysis status, diagnostics, solver budgets;
- `SetTyping.hpp`: finite-set typing constraints solved by Satie SAT;
- `StructuralTyping.hpp`: structural type unification over Satie EUF;
- `Language.hpp`: a shared sample AST and set/structural type checkers;
- `tests/set_typing.cpp`: original tests.

These existing components are **not replaced**. This deliverable adds an independent
`sequent/` module, an opt-in `Bridge.hpp`, and `TypeAdapters.hpp`, plus its own
CMake tests, examples, manuals and an updated umbrella header.

**Archive transport limitation:** the ZIP contains the new/modified files, not the
five unchanged upstream files. This is a drop-in overlay for the repository.
If assembling a standalone full snapshot, run `python3 tools/restore_upstream.py
--checkout /path/to/libstone` or `python3 tools/restore_upstream.py --fetch`
(from a network-enabled machine). The helper pins/validates upstream Git blob SHAs.
No original Satie-based code is faked or silently substituted.

## Features implemented

- Separate **terms**, **continuation frames**, **commands**; cut pairs `<v || k>`.
- `mu ret.c` / lexical `ret`, typed lambda and type-lambda terms, constructor values.
- Multi-input join bindings with optional type parameters, typed direct jumps.
- Named data constructors, exhaustive Boolean/ADT cases, `let` and recursive `let rec`.
- Typing/scoping verifier, term continuation duality, ban on free ambient labels in terms.
- Call-by-need **reference** evaluator with memoized thunks, constructor field laziness,
  lexical jumps, black-hole detection and step budget.
- Simplification via mu reduction/continuation substitution, sharing-preserving beta
  reduction, known-constructor branch reduction, unused-let and unused-join elimination.
- **Conservative contification:** exact fully saturated tail-only monomorphic functions
  returning to the same ret are demoted into join bindings and jumps.
- `optimize_checked` rejects invalid transformations, readable printer, metrics, DOT
  tree visualization, and deterministic tests.
- An optional `AstBridge` for the existing `ast::Expr` and adapters for ground
  structural and singleton set-typing results.

## Quickstart (new IR only; Satie not needed)

```sh
cmake -S nerotypical -B nerotypical/build -DCMAKE_BUILD_TYPE=Debug
cmake --build nerotypical/build -j
ctest --test-dir nerotypical/build --output-on-failure
c++ -std=c++20 -I nerotypical/include nerotypical/examples/joins.cpp -o /tmp/nt-joins
/tmp/nt-joins
```

The original suite additionally needs the full LibStone checkout and vendored
`third_party/satie` on the include path. `examples/from_ast.cpp` also needs those.

## Embedding

```cpp
#include <nerotypical/sequent/SequentIR.hpp>
namespace sc = moonstone::nerotypical::sequent;
namespace m = moonstone::nerotypical::sequent::make;
sc::Type int_type = sc::Type::integer();
sc::Module program;
program.result_type = int_type;
program.entry = m::cut(m::integer(42));
sc::Module optimized = sc::optimize_checked(program);
auto value = sc::Evaluator{}.run(optimized);
```

## Important limits

This is a useful IR implementation, not a complete reimplementation of the
Downen–Maurer–Ariola–Peyton Jones Sequent Core research compiler.

- No complete System Fω kind system, type equalities/casts, GADTs, unboxed values,
  coercions, proof-producing optimizer, or arbitrary control effects.
- Explicit `forall` and type application are supported, but constructors and
  recursive types are **monomorphic** in this release. The checker rejects
  accidental type-variable scope escapes but does not implement full type inference.
- `AstBridge` requires a frontend-supplied inferred-type oracle; it does **not**
  claim to invoke the Satie solver automatically. It represents source strings
  via an explicit text callback because the upstream demo AST drops literal text.
- The interpreter uses C++ recursion and a fuel bound; it is not a production VM.
  Native `$nt.*` operators are intended for testing/evaluation, not sandboxing.
- The optimizer does **not** include the paper's full continuation shrinking,
  recursive contification or production cost model; heavy case-of-case can duplicate
  continuations. Apply cost budgeting before production deployment.
- `Eval` requires no unresolved external globals except explicitly registered
  `$nt.*` intrinsics; user-defined externals need their own runtime bindings.

See `manual/` and `AGENTS.md` for exact invariants and integration milestones.
