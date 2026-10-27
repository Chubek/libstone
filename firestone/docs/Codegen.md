# Instruction selection and scheduling

`Firestone::isel` and `Firestone::isched` are header-only C++17 components.
They contain original, dependency-free implementations: LLVM, Z3, and external
integer-programming solvers are not required. Include an individual algorithm
header or `Firestone.hpp`.

## Shared pipeline

```cpp
#include <libisel/BURS.hpp>
#include <libisched/ListSched.hpp>

firestone::ir::DataflowGraph ir;
auto a = ir.constant(7);
auto b = ir.constant(9);
auto sum = ir.operation(firestone::ir::Opcode::Add,
                        firestone::ir::Type::integer(), {a, b});
auto graph = firestone::isel::from_dataflow(ir);
firestone::isel::Rules rules;
firestone::isel::Rule literal;
literal.name = "literal";
literal.pattern = firestone::rewrite::parse("constant");
literal.instruction = "MOV_IMM";
rules.add(literal);
auto add = literal;
add.name = "add";
add.pattern = firestone::rewrite::parse("(add ?lhs ?rhs)");
add.instruction = "ADD";
rules.add(add);
auto selected = firestone::isel::BURS{}.select(graph, rules, {sum});
auto operations = firestone::isched::from_selection(selected);
auto schedule = firestone::isched::ListSched{}.schedule(operations);
auto errors = firestone::isched::verify(operations, {}, schedule);
```

A selection graph is an append-only DAG. Node IDs are stable and children must
already exist. `from_dataflow` preserves the original typed instructions, including
immediates, symbols, and annotations, in `Node::source`; opcode names are lowercase
IR names. `Graph::import` consumes `librewrite::Term` trees. `from_extraction`
consumes `libeqsat::DAGExtraction` without losing sharing; use its original root ID.
This permits equality saturation, extraction, instruction selection, and scheduling
without converting to a second unrelated expression representation.

Rules match `librewrite` patterns. Variables are named leaves, and repeated
variables require the same node ID. Guards receive the graph, root ID and bindings
for target-specific type/immediate legality. A rule emits one instruction, with
nonnegative finite cost, positive latency and a named resource. Different target
operand forms can be expressed by different rules and guards. Variables are
ordered lexicographically in emitted inputs; `input_names` preserves their names.
The selected root and covered node IDs refer back to the original graph, so a
backend can recover immediates and types for encoding. These libraries do not
encode machine bytes or invent ISA-specific semantic equivalences.

Only nodes reachable from the requested roots are selected. Tiles may absorb a
node only when all of its users are inside that tile, and requested outputs may
never be absorbed. Shared values exposed outside a tile therefore stay explicit.
There is no implicit recomputation, multi-result instruction, or duplication of
effects. This restriction defines the legal cover space for all exact solvers.

## Selection algorithms

| Header | Behavior |
| --- | --- |
| `MacroExpansion.hpp` | Ordered, guarded term macro expansion using `librewrite` substitutions and bounded rewriting; exposes rewrite termination status. |
| `PeepholeOpt.hpp` | Bounded fixed-point rewriting of instruction-term windows, with bindings shared across the window; supports deletion, guards and no-op detection. Callers encode operand/effect legality in rules. |
| `MaximalMunch.hpp` | Top-down largest feasible tile first; cost breaks equal-size ties. |
| `DynamicProg.hpp` | Memoized minimum-cost tree recurrence, followed by cover reconstruction. |
| `DAGDynProg.hpp` | Topological dynamic programming with one emission per shared value; tree-cost estimates are a heuristic on DAGs. |
| `BURS.hpp` | Bottom-up rewrite labeling, normalized production-cost states and a top-down reducer. `label(Problem)` exposes states, costs and winning tiles. One value nonterminal; types use guards. Runtime labeling, not a generated matcher compiler. |
| `GraphCovering.hpp` | Exact-cover branch-and-bound, most-constrained uncovered node first, seeded with a feasible DP incumbent. |
| `ILP.hpp` | Binary cover model (`formulate`) and an internal branch-and-bound solver. Every live node has an exactly-one coverage constraint. |
| `UniversalSMT.hpp` | Finite selection constraints with Boolean tile choices, exact-cover cardinality, extra signed-literal clauses and branch-and-bound cost minimization. Guards discharge target predicates before solving; this is not a general-purpose SMT theorem prover. |
| `TreeDecomposition.hpp` | Exact factor-table dynamic programming via min-degree variable elimination on the cover constraint graph. Elimination bags are bounded explicitly. |

`Selection::stop` distinguishes `Complete`, `Infeasible`, and `SearchLimit`.
A limited branch-and-bound search may contain a feasible incumbent; an empty
instruction vector with nonempty roots is not a feasible answer. `optimal` is
true only when proved for the specified model. Tree methods do not claim DAG
optimality. `explored` reports search nodes where applicable. Bounds for factor
elimination limit bag size, while other exact selectors bound visited search
nodes. Exact solvers are intended for modest regions; heuristics work on larger
ones. Pattern matching and tree traversal currently use recursive C++ calls.

## Scheduling model

An `isched::Graph` contains stable operation IDs and explicit dependencies.
Dependencies encode minimum start-time separation, kind (data, anti, output,
memory or control) and optional iteration distance. All data, alias, exception,
predicate, and ordering dependencies relevant to the target must be present.
Latency is result availability; occupancy is functional-unit reservation length.
An operation can reserve multiple units of one resource. Machine resources and
issue width have positive capacities. Resource names must resolve on the machine.
No latency, throughput or alias assumptions are imported from LLVM or a host CPU.

`from_selection` maps selected instructions to operation IDs, retains original
selection IDs in `Operation::source`, and creates latency-weighted input edges.
`from_ssa_block` checks the source SSA graph, treats phi results as block-entry
inputs, chains memory/call/division/remainder operations conservatively, and pins
terminators after other operations. A callback supplies target operation costs
and resources. The returned graph is a plan and does not mutate SSA.
`register_dependencies` builds RAW/WAR/WAW edges from register read/write lists;
callers add memory/control edges separately.

`Schedule::start` is indexed by operation ID; `order()` returns stable cycle order.
Makespan is the latest result completion (not the end of all resource occupancy).
`verify` independently checks completeness, dependency delays, occupancy, issue
width and makespan. Search-limited exact schedulers retain a feasible list-schedule
incumbent. Cycles and loop-carried dependencies are rejected by ordinary schedulers.
Time arithmetic is checked for overflow.

| Header | Behavior |
| --- | --- |
| `LocalSched.hpp` | Earliest-ready list scheduling with stable source-order ties. |
| `ListSched.hpp` | Earliest-ready scheduling with critical-path priority. |
| `SelectionDAGSched.hpp` | Critical-path priority with a single-use-input pressure tie-breaker. This does not compute physical-register allocation. |
| `ProfileDrivenSched.hpp` | Frequency-weighted critical-path priority. |
| `VLIWPacking.hpp` | Resource-valid cycle bundles; empty cycles are implicit. The model has resource classes, not target-specific slot encodings. |
| `AntColonyOpt.hpp` | Seeded stochastic priority search with pheromone evaporation and reinforcement of the best schedule. |
| `IntegerProgSched.hpp` | Time-indexed binary ILP formulation (`formulate`) and lazy finite-domain enumeration across makespan bounds. |
| `ConstraintProgSched.hpp` | Exact finite-domain search with dependency propagation and smallest-ready-domain branching. |
| `ModuloSched.hpp` | Bounded periodic scheduling with loop-distance inequalities, modulo resource occupancy and issue constraints. Searches increasing initiation intervals. |
| `LoopUnrolling.hpp` | Replicates a dependence graph, remaps intra-chunk edges, retains carried distances across chunks and records original ID/iteration pairs. |
| `OutOfOrder.hpp` | In-order issue and retirement, bounded reorder window, out-of-order execution, register renaming and bounded writeback. |
| `ScoreBoarding.hpp` | In-order issue, functional-unit admission and preserved RAW/WAR/WAW dependencies, followed by out-of-order execution. |
| `Tomasulo.hpp` | Tagged data dependencies, renaming of WAR/WAW, bounded reservation capacity and common-data-bus writeback. This variant includes in-order retirement. |
| `Speculative.hpp` | Relaxes control dependencies only for explicitly speculatable destinations; reports relaxed edges and the graph actually scheduled. |
| `TraceSched.hpp` | Profile-guided acyclic hot-path discovery and conservative trace-region scheduling with block boundaries retained. |
| `SuperblockSched.hpp` | Single-entry hot-path discovery and checked region scheduling; allows explicitly safe speculative motion. |
| `HyperblockSched.hpp` | Scheduling of caller-if-converted regions with explicit predicates; retains predicates and removes eligible control edges. |
| `DelaySlotSched.hpp` | Moves independent, explicitly nontrapping operations from before a branch into architectural delay slots; unfilled slots are explicit optionals. |

Regional APIs return plans; they do not perform tail duplication, compensation
code insertion, if-conversion, or CFG rewriting. Trace regions with side entrances
retain boundaries. Superblocks require a single-entry path. Predicates in a
hyperblock must already be executable target predicates, with their definition
edges included. `speculatable` is an explicit guarantee of no traps or observable
effects, never inferred merely from an opcode name. Delay-slot filling transforms
instruction order, and target timing must be checked after final placement.

Dynamic simulators require IDs in program order and forward dependencies. Execution
start, issue, completion/writeback and retirement times are separate. Renamed
schedules should be verified against `detail::renamed(graph)` if the source graph
contains WAR/WAW constraints. Memory and control edges are never removed by
renaming. The simulators model scheduling, not execution of instruction values.

Modulo scheduling returns the kernel and initiation interval, with a dedicated
`verify` overload for periodic occupancy and carried dependencies. Its finite
stage and search bounds mean failure is `SearchLimit`, not proof of infeasibility;
a successful result does not claim global optimality outside those bounds.
Unrolling is a dependence-graph transformation: loop control, induction updates,
remainder loops, and prologue/epilogue machine code remain the backend's job.
