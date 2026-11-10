# Wetstone — LibStone static analysis framework

Wetstone is an implementation of a C++20 static-analysis layer for LibStone. It is designed to be installed at `libstone/wetstone/` and to integrate with Flintstone and Firestone when their headers are available. The repository's original seven directories contained empty headers; this release provides working implementations of their interfaces, additional analyses and domains, and new `interprocedural/`, `verification/`, `models/`, `integration/`, `tests/`, `examples/`, and `docs/` directories.

## What is implemented

- **Abstract interpretation:** interval, constant, sign, parity, congruence (including CRT intersection), finite powersets, known bits, unsigned bitvector intervals with full arithmetic and comparisons, a typed-integer reduced product (bitvector interval ⊗ known bits), strided intervals, reduced products, finite relational DBMs, octagons with strong closure, a restricted affine-equality domain, Boolean predicate maps, disjunctive domains, bounded trace partitions, widening with thresholds, interval environments, transfer functions, and fixpoint iteration with optional narrowing.
- **Dataflow:** generic worklist solvers in either direction, reaching definitions, liveness, definite assignment, available expressions, sparse conditional constant propagation, local value numbering, def-use chains, and backward slicing.
- **Path/CFG:** reachability, reverse postorder, dominance, immediate dominators, frontiers, postdominance, natural loops, SCCs, bounded path enumeration, branch feasibility, bounded path-sensitive abstract execution, taint tracking, and control-dependence estimates.
- **Pointer:** flow-insensitive inclusion-based Andersen points-to, object/variable constraints, load/store/copy modeling, alias classification, escape detection, simple nullness propagation, callsite collection, effects, and single-block double-release diagnostics.
- **Symbolic:** typed expression trees, evaluation, SMT-LIB2 generation, Boolean Tseitin encoding, conditional Satie CDCL solving, Satie bounded LIA integration, bounded symbolic path exploration, and proof obligations.
- **Repair:** arithmetic/ownership repair candidates, SARIF diagnostics, and SAT-guided candidate-set selection when Satie is present. Suggestions are **not** automatically sound source patches.
- **Interprocedural:** direct call graph, recursive SCCs, context call strings, finite effect summaries with conservative external-call behavior, external function models, and analysis caching/orchestration.
- **Equality saturation:** an optional adapter to `equinoxng::EGraph::add/run/extract` using the Equinox-NG rewrite and cost model APIs.

## Build and test

```sh
# From the LibStone repository root (overlay this directory first):
cmake -S wetstone -B build-wetstone -DCMAKE_BUILD_TYPE=Release
cmake --build build-wetstone --parallel
ctest --test-dir build-wetstone --output-on-failure
```

There are **no fetched dependencies**. By default the build looks for Satie under `third_party/satie/include/`, Equinox-NG under `third_party/equinox-ng/` and LibStone's own Firestone/Flintstone headers. Override the vendor root with `-DWETSTONE_THIRD_PARTY_DIR=/path/to/third_party`.

For a standalone build without the parent LibStone tree, only the core headers and tests are required. SAT/LIA/Equinox integration will be unavailable until you supply their directories. You can disable optional search using `-DWETSTONE_USE_SATIE=OFF -DWETSTONE_USE_EQUINOX=OFF -DWETSTONE_USE_FLINTSTONE=OFF`.

The implementation is *header-only*; include `Wetstone.hpp` or individual headers. For LibStone root builds, add `add_subdirectory(wetstone)` to the root CMake project and link `Wetstone::wetstone` in clients.

## A first analysis

```cpp
#include "wetstone/Wetstone.hpp"
using namespace wetstone;

ir::Function f;
f.name = "check";
f.entry = 0;
f.parameters = {0};
f.blocks = {{0, {
    {ir::Opcode::Constant, 1, {ir::Operand::constant(42)}},
    {ir::Opcode::Add, 2, {ir::Operand::ref(0), ir::Operand::ref(1)}},
    {ir::Opcode::Return, {}, {ir::Operand::ref(2)}}
}}};

f.validate();
auto result = ai::analyze_intervals(f);
if (!result.converged) { /* budget exhausted: result is not a fixpoint */ }
```

## Soundness and scope contracts

1. **Semantics:** `ai::Interval` and the scalar arithmetic transfer functions model mathematical integer operations *assuming results stay in representable signed 64-bit range*. They do **not** prove absence of modular wraparound or signed-overflow undefined behavior. Flintstone's built-in arithmetic is explicitly modular, so the default Flintstone adapter conservatively maps affected arithmetic to **Top** (`Opcode::Input`) rather than falsely analyzing it as mathematical integers. Use a typed bitvector domain to recover precision.
2. **Numerical limits:** no general convex-polyhedra engine is claimed. DBMs and octagons are concrete difference-constraint representations; `AffineEqualities` implements a restricted copy-plus-offset fragment, not the full Karr algorithm.
3. **Pointers:** Andersen models known allocation sites and inclusion constraints. Unknown calls and pointer inputs are treated conservatively, but no field-, heap-context-, or flow-sensitive completeness is claimed. Alias `MustAlias` is valid only under the model's finite non-unknown object universe.
4. **Symbolic:** the built-in Boolean encoder accepts true Boolean formulas. Nontrivial integer theory constraints are **never** booleanized as independent uninterpreted atoms: they return `Unknown` and can be exported through SMT-LIB2 or solved through the explicit Satie LIA interface.
5. **Budgets:** fixpoint and path analyses return `converged=false` or `truncated=true` on bound exhaustion. Do not interpret an incomplete result as a proof. Repair candidates and warning scans are heuristics, not verified patches.
6. **Optional sources:** LibStone's GitHub `master` did **not** track a `third_party/` directory when this work was made, despite documenting it as a local vendoring location. The ZIP therefore contains **no copied vendor libraries**. These adapters were written against public upstream APIs and were not compiled against a full local vendor checkout.
7. **Portability:** the arithmetic implementations use the GCC/Clang `__int128` extension to avoid intermediate signed overflow. They require an implementation that supports that extension.

See `docs/ARCHITECTURE.md`, `docs/DOMAINS.md`, `docs/SOLVERS.md`, `docs/INTEGRATION.md`, and `docs/TESTING.md` for details.
