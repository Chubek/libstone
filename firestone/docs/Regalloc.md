# Register allocation

`Firestone::regalloc` is a dependency-free, header-only C++17 component in
`firestone::regalloc`. Include individual `libregalloc/*.hpp` headers or
`Firestone.hpp`. The following **20 methods** are selected from `REGALLOC.md`;
supporting techniques have their own APIs rather than pretending to be allocators.

| # | Header | Implemented method |
| --- | --- | --- |
| 1 | `ChaitinRegalloc.hpp` | Simplify/select graph coloring with pessimistic removal of a low spill-cost/degree candidate when simplification stalls. |
| 2 | `BriggsRegalloc.hpp` | Optimistic simplify/select: potential spills remain on the select stack and spill only if assignment fails. Shared implementation: `GraphColoringRegalloc`. |
| 3 | `KempeChainRegalloc.hpp` | Bounded two-color connected-component swaps to recover spilled values. Checks precolors, domains and aliases before committing a swap. |
| 4 | `CoalescingRegalloc.hpp` | Copy-affinity contraction followed by coloring. Conservative mode uses Briggs's high-degree-neighbor criterion on uniform domains; optional aggressive mode contracts any legal noninterfering pair. |
| 5 | `LinearScanRegalloc.hpp` | Start-ordered convex-interval allocation, expiration through overlap checks, furthest-end eviction and whole-value spilling (Poletto–Sarkar style). |
| 6 | `ExtendedLinearScanRegalloc.hpp` | Hole-aware interval allocation with a second placement pass for spilled/evicted values; preserves future conflicts with inactive intervals. |
| 7 | `IntervalSplittingRegalloc.hpp` | Split at caller-selected positions, allocate fragments independently, and return required location transfers. |
| 8 | `SSAChordalRegalloc.hpp` | Maximum-cardinality search, explicit perfect-elimination verification, then reverse-elimination coloring. Rejects nonchordal input. |
| 9 | `GreedyRegalloc.hpp` | Priority allocation, bounded transactional recoloring, aggregate-cost eviction and a second-chance pass. Priorities include spill cost, use density, frequency, loop depth and degree. |
| 10 | `LocalNextUseRegalloc.hpp` | Straight-line Belady-style furthest-next-use eviction with dirty tracking, reload/store actions and live-out flushing. |
| 11 | `PBQPRegalloc.hpp` | Inspectable unary/pairwise cost model and bounded exact finite-domain branch-and-bound. Infinite pair costs encode interference. |
| 12 | `ILPRegalloc.hpp` | Binary assignment formulation: exactly-one rows, conflict inequalities and affinity-product variables. Uses the equivalent PBQP finite-domain solver internally. |
| 13 | `BipartiteMatchingRegalloc.hpp` | Minimum-cost simultaneous-live-set assignment via successive augmenting paths in a residual flow network; private spill vertices and disjoint physical registers. |
| 14 | `SimulatedAnnealingRegalloc.hpp` | Seeded stochastic assignment/spill proposals, legal eviction and cooling, retaining the best feasible result. |
| 15 | `SSABasedSpilling.hpp` | SSA-aware allocation and explicit spill/rematerialization actions at definitions, uses, entry parameters and phi edges. |
| 16 | `LiveRangeSplitting.hpp` | Range partitioning, original/fragment maps, inherited domains, conservative explicit-edge propagation and boundary records. |
| 17 | `ParallelCopyResolution.hpp` | Sequentialize simultaneous copies, including cycles and fan-out, with one caller-provided scratch location. |
| 18 | `SpillSlotColoring.hpp` | Reuse noninterfering stack slots while preserving size/alignment requirements. Also runs during allocation finalization. |
| 19 | `RegisterScavenging.hpp` | Find a free temporary register or a least-cost set of live values to save/restore, respecting alias units and nonspillable values. |
| 20 | `RegisterPressure.hpp` | Half-open interval sweep, total peak live-value count and per-register-class pressure profiles. |

`Liveness.hpp` integrates existing `libir` phi-aware liveness and interference
analysis. `BranchAndBoundRegalloc` is an alias for the PBQP solver, not another
independent implementation. Original populated abbreviated headers such as
`BasicLS.hpp`, `BriggsGC.hpp` and `IntervaSplittingLS.hpp` are compatibility entry
points. Their canonical names above should be used for new code. The original
empty George–Appel and regional/trace headers are not implementations.

## Example

```cpp
#include <libregalloc/GreedyRegalloc.hpp>
#include <libregalloc/Liveness.hpp>

firestone::ir::SimpleSSAGraph ssa;
auto block = ssa.cfg().create_block("entry");
auto input = ssa.parameter(firestone::ir::Type::integer());
auto result = ssa.define(block, firestone::ir::Opcode::Add,
    firestone::ir::Type::integer(),
    {firestone::ir::Operand::ref(input), firestone::ir::Operand::constant(1)});
ssa.cfg().return_value(block, firestone::ir::Operand::ref(result));

auto problem = firestone::regalloc::from_ssa(ssa);
auto machine = firestone::regalloc::Machine::uniform(4);
auto allocation = firestone::regalloc::GreedyRegalloc{}.allocate(problem, machine);
auto errors = firestone::regalloc::verify(problem, machine, allocation);
```

```cmake
find_package(Firestone CONFIG REQUIRED COMPONENTS regalloc)
target_link_libraries(my_backend PRIVATE Firestone::regalloc)
```

## Shared model

`Problem::add(Value)` returns a dense stable value ID. Values can have sorted,
nonoverlapping half-open `ranges` and sorted `uses`. Overlapping ranges add
interference automatically; `interfere(a, b)` adds explicit edges. Graph-only
values can omit ranges. Interval and pressure algorithms require ranges.
`prefer(a, b, weight)` adds a soft copy affinity, not permission to ignore an edge.
All tie-breaking is stable by value/register ID unless a priority or hint applies.

Each physical-register ID identifies one allocation choice. `units` models
overlapping storage: a full register and its subregisters share units, while a
pair/tuple contains multiple units. An empty unit set means the register's own
ID. Simultaneously live choices may never overlap units. Classes, banks, reserved
registers, an optional allowed-register set, and fixed assignments constrain every
allocator. An absent allowed set is unrestricted; an empty set forces spilling
or failure. Fixed assignments cannot spill. Register hints are soft preferences.

Target backends provide these descriptions explicitly. For example, a value live
across a call can exclude caller-clobbered registers using `allowed`; fixed operands
can be represented by constrained fragments. There is no built-in ISA/ABI table.
`callee_saved` is metadata; `PhysicalRegister::cost` is a per-value assignment cost,
not a once-per-function callee-save optimization. Register units can express GPU,
SIMD, predicate, bank and tuple restrictions without separate allocator algorithms.

`spill_cost`, `frequency` and rematerialization costs must be finite and nonnegative.
`rematerialization_cost` is an explicit backend guarantee that recomputation is
valid. A spill chooses the cheaper of memory and rematerialization. The common
objective is register assignment costs plus spill/rematerialization costs plus
affinity penalties for endpoints that do not share the same physical-register ID.
Heuristics aim for useful assignments rather than minimizing that complete model.
Costs use `double`; overflow is rejected.

`Allocation::locations` is indexed by value ID. A location is a register, a logical
stack-slot index, a rematerialization marker, or unassigned. Stack slots have size
and alignment requirements; frame offsets are assigned by the backend. `verify`
checks domains, precolors, aliases, interference, stack-slot reuse, completeness
and the reported objective independently of the allocator's placement decisions.

`feasible` identifies a usable incumbent. Statuses are:

* `Complete`: allocation completed, possibly with spills.
* `NeedsRepair`: a heuristic could not place a nonspillable value; no infeasibility
  proof is claimed.
* `Infeasible`: an exact method proved no solution in its model.
* `SearchLimit`: the exact solver exhausted its node budget. A feasible incumbent
  may still be available; inspect `feasible` and verify it.

`optimal` is set only by completed exact search/matching. Chordal coloring is
spill-free when its unconstrained, uniform-register assumptions and sufficient
register count hold; this does not imply optimal weighted spilling with irregular
domains or precolors. `explored` reports exact-search nodes or annealing iterations.
Invalid inputs, including conflicting precolors, raise `std::invalid_argument`.

## SSA, splitting and lowering

`analyze_ssa` returns the problem, block-entry/exit liveness and position maps.
Instruction read and write positions are distinct; a dying input may share a
register with its result. Phi uses occur on predecessor exits. SSA value IDs are
preserved. A configuration callback supplies target classes, costs, hints, fixed
assignments and rematerialization guarantees. The adapter includes all CFG blocks
and uses the existing SSA verifier before analysis.

`SSABasedSpilling::plan` returns abstract actions, not rewritten machine code.
`StoreDefinition` occurs after the definition (including phi lowering),
`StoreParameter` at entry, and reload/rematerialization before a use. An action
with `successor` is on that outgoing CFG edge and may require critical-edge
splitting. The backend supplies instruction encodings, scratch registers,
rematerialization recipes, parallel phi copies and any subsequent reallocation.
Repeated normal operands within one instruction share one reload action.

Splitting returns a new problem and an allocation indexed by **fragment** IDs.
Cuts must be sorted, unique and strictly inside the value's bounding interval.
Spill costs are apportioned by live length; no move cost is silently added.
Explicit interference edges lack positions, so every resulting fragment pair
inherits them conservatively. `SplitAllocation::transfers` records changes at
boundaries. Backend lowering must place and resolve these transfers before using
the allocation. Lifetime holes describe CFG liveness, not permission to discard
a value that must survive an intervening straight-line clobber. Transfers across
holes/CFG edges need edge-specific placement and preservation by the backend;
the position-only plan does not reconstruct that control flow.

The current interval implementations use the shared interference graph to test
active/inactive conflicts; they do not promise the asymptotic complexity of an
optimized interval-tree allocator. Convex linear scan intentionally fills holes
and can reject precolors that conflict only after taking their interval hulls;
use the hole-aware variant for such inputs.

## Other method contracts

* Conservative coalescing applies its safety criterion only to identical,
  nonaliasing domains. Other constraints retain affinity-guided placement without
  contraction. Aggressive coalescing may increase spilling.
* The ILP model's `products` encode `y = x_left AND x_right`, with the usual
  `y <= x_left`, `y <= x_right`, `y >= x_left + x_right - 1` linearization.
  Its objective is `constant + unary costs - product savings`. PBQP and ILP share
  an exact solver; neither requires an external optimization library.
* Matching requires an interference clique and disjoint registers. It is optimal
  for that simultaneous-live-set assignment model, including independent spills.
* Local next-use allocation takes single-definition straight-line instructions.
  All operands and results of an instruction coexist. Live-ins initially reside
  in value-ID-indexed homes; emitted actions execute in their returned order
  before the corresponding instruction. Final stores have instruction index
  `instructions.size()`. The result is a local plan, not a global `Allocation`.
* Parallel-copy locations must be atomic, nonaliasing storage cells, with unique
  destinations and a scratch cell distinct from every operand. The backend
  legalizes memory-to-memory moves and physical subregister aliases.
* Scavenging returns save/restore obligations; it does not emit saves or clobber
  precolored/nonspillable live values. Its `live` set is supplied by the caller.
* Pressure counts live values per class; it is not a proof that an irregular
  register-file assignment exists or a weighted count of tuple units.

Tests cover exhaustive small-graph optimality, constrained and aliased registers,
spilling, holes, splitting, coalescing, Kempe swaps, SSA phi-edge plans, executable
local and parallel-copy plans, malformed inputs, search limits and installation.
