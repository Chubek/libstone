# Novostone

Executable compiler-method libraries based on
`/mnt/aleph/dossier-on-modern-cc-design/` as the design source of truth. The six components form a small, testable
compiler substrate with relational analysis, transactional optimization,
speculation/recovery, and learned profitability decisions.

## Components and dependencies

| CMake target / directory | Implementation | Dossier basis |
| --- | --- | --- |
| `Novostone::Datalog` / `Datalog` | Typed relations, indexed delta joins, semi-naive recursive fixed points, stratified negation, dependencies, budgets and counters | Chapter 07's relational analysis substrate |
| `Novostone::CCDB` / `CCDB` | Stable IDs, immutable snapshots, serializable transactions, tracked disjoint rebasing, dependency-aware caches, checkpoints and audit log | Chapters 07, 20, 22, 30 |
| `Novostone::DatabaseBasedIR` / `DatabaseBasedIR` | Relational SSA, edge-checked phis, dominance, liveness, natural loops, local memory chains, reference evaluator | Chapters 07, 16, 17; system blueprint |
| `Novostone::QueryBasedIR` / `QueryBasedIR` | Declarative rewrites, conditional constant propagation, rooted DCE, CFG simplification, CSE, forwarding, differential validation | Chapters 09, 39 |
| `Novostone::SpeculativeIR` / `SpeculativeIR` | Explicit assumptions, guards, frame snapshots, parameter specialization, invalidation, resumable deoptimization | Chapters 03, 04 |
| `Novostone::CostCC` / `CostCC` | Target-specific ridge regression, calibration, model persistence, drift quarantine, fallback, ranking and measurement logs | Chapters 11, 27 |

Dependency order:

```text
Datalog -> CCDB -> DatabaseBasedIR -> QueryBasedIR -> CostCC
                                 -> SpeculativeIR
```

All IR libraries use the same CCDB/Datalog implementation. No external database,
solver, package download, or machine-learning runtime is required. Public headers
live at `novostone/<component>/<component>.hpp` in namespace `novostone`.

## Build, test, and run

```sh
cmake -S novostone -B build-novostone -DCMAKE_BUILD_TYPE=Debug
cmake --build build-novostone --parallel
ctest --test-dir build-novostone --output-on-failure
build-novostone/novostone demo
build-novostone/novostone optimize novostone/examples/wrapping.nir
build-novostone/novostone run novostone/examples/sum-loop.nir 10
build-novostone/novostone analyze novostone/examples/sum-loop.nir
build-novostone/novostone optimize novostone/examples/equal-phi.nir
build-novostone/novostone validate novostone/examples/wrapping.nir novostone/examples/wrapping.nir 728
```

The aggregate build enables Novostone through `LIBSTONE_BUILD_NOVOSTONE`.
Standalone options are `NOVOSTONE_BUILD_TESTS` and `NOVOSTONE_BUILD_TOOLS`,
defaulting to the shared Libstone options. C++20 and CMake 3.20 are required.
`BUILD_SHARED_LIBS=ON` builds shared libraries; Unix installations use relative
runtime paths so the CLI and transitive libraries resolve within the prefix.

Installation exports a relocatable CMake package:

```sh
cmake --install build-novostone --prefix /your/prefix
```

```cmake
find_package(Novostone CONFIG REQUIRED)
target_link_libraries(my_compiler PRIVATE Novostone::QueryBasedIR Novostone::SpeculativeIR)
```

## Build and optimize a function

```cpp
#include <novostone/QueryBasedIR/QueryBasedIR.hpp>
using namespace novostone;

ccdb::Database database(ir::program(), [](const auto& s) { ir::verify(s); });
ir::Builder builder(database, 1); // one i32 argument
auto block = builder.block();
auto x = builder.emit(block, ir::Op::Param, {}, 0);
auto zero = builder.emit(block, ir::Op::Const, {}, 0);
auto sum = builder.emit(block, ir::Op::Add, {x, zero});
builder.emit(block, ir::Op::Return, {sum});
auto source = builder.finish();

auto uses = source.query("Uses"); // rows, dependencies, revision, cache status
auto candidate = query::optimize(source);
auto optimized = query::apply(database, candidate); // rejects a stale source
auto result = ir::evaluate(ir::load(optimized), {42});
// result.value == 42; candidate.explain() describes each rewrite
```

`ir::Builder` constructs one function; the first block is its entry. Its blocks
can be allocated before emitting branches. `ir::store` writes a complete verified
function into an open transaction. `ir::publish` also checks the expected source
snapshot. Imported block/instruction IDs are reserved against future allocation.

## Semantic contract

The implemented dialect has one value type: a 32-bit integer bit pattern.
`add`, `sub`, and `mul` wrap modulo 2^32; `eq` and unsigned `ult` produce 0 or 1.
`select` and `branch` treat any nonzero condition as true. There is no poison or
implicit undefined overflow. Constants and parameter indices use `immediate`;
other operations require an immediate of zero.

`load(address)` and `store(address, value)` access an explicitly supplied vector
of i32 cells. Addresses are cell indices, not host pointers. Invalid accesses
produce `memory bounds` before performing the access. Stores are recorded in an
ordered observation trace. Loads are potentially trapping and are never treated
as dead pure operations. Instructions execute in block order. Rewrites never move
or duplicate stores; load removal requires prior successful-access and alias
evidence (see below). The argument count must exactly match the
function signature, even if optimization removes an unused parameter.

Blocks end in `jump`, `branch`, or `return`. The verifier checks unique positive
entity IDs, operand/target arity, reachable blocks, terminator placement, defined
values, and dominance including same-block definition order. Parameters belong
to the entry block. Leading `phi` instructions select values by predecessor edge,
with all phis assigned **in parallel**. Every CFG predecessor appears exactly
once, and each incoming value must dominate its source edge. Entry phis,
duplicate predecessors, and misplaced phis are rejected. Loop-carried data no
longer needs artificial heap traffic. Execution has a caller-controlled step budget. Budget
exhaustion is a host resource limit, not a language exception; optimization can
change instruction counts.

`ir::Outcome` reports a returned value or deterministic trap, memory, ordered
writes, and execution state. `ir::equivalent` compares semantic observations,
excluding SSA temporaries and step counts. `ir::resume` is the low-level recovery
API: its supplied PC and live values must be valid for that function.

Construct cyclic phis before their backedge values, then bind the edges:

```cpp
auto counter = builder.emit(header, ir::Op::Phi);
// ... emit the loop body and its `next` value ...
builder.incoming(counter, entry, zero);
builder.incoming(counter, body, next);
```

`Instruction::operands[k]` and `targets[k]` describe a phi's incoming value and
predecessor. These records are not CFG successor edges. `State` retains the
predecessor and captured parallel phi inputs, so suspension/resumption between
phi assignments preserves cyclic swaps.

`ir::effects` exposes read/write/trap summaries. `ir::memory_dependencies` produces
a per-block single-state chain: each access names the preceding memory access
(zero denotes block entry). These are local dependence edges, not global
MemorySSA or an alias proof across control-flow merges.

### Global SSA analyses

`ir::liveness(snapshot)` provides block live-in/live-out, edge live sets, and
instruction before/after sets tagged with the source revision. The corresponding
`LiveIn`, `LiveOut`, `LiveEdge`, `UseAt`, `BlockDef`, and `PhiUse` relations are
queryable through CCDB. Phi operands are uses on their **incoming edges**, not
uses in the join block; this avoids making values live on unrelated predecessors.
All phis share one parallel before/after boundary in instruction-level results.
Recovery `Binding` values are also live at their guard, even without ordinary
SSA users. Limits passed to `liveness` apply separately to each analysis query.

`ir::liveness(function)` analyzes a core function without recovery metadata.
`ir::natural_loops(function)` returns dominance backedge headers, latches, member
blocks, and exit edges. Multiple latches with the same header are merged; nested
loops remain distinct. Irreducible cycles are not claimed to be natural loops.
`Liveness::explain()` and the CLI `analyze FILE` expose the resulting facts.

### Text format

`ir::dump` and `ir::parse` round-trip the verified core function:

```text
novostone VERSION ARITY ENTRY_BLOCK
block BLOCK_ID
instr ID OPCODE IMMEDIATE OPERAND_COUNT OPERANDS... TARGET_COUNT TARGETS...
```

Version 1 retains the original phi-free grammar; version 2 additionally permits
phi operand/predecessor lists. The dumper selects version 2 only when needed.
The parser accepts both and verifies the full CFG.

The CLI's `optimize` command writes this format to stdout and rewrite explanations
to stderr. CCDB's separate `Snapshot::dump()` includes revision and all primary
relations, including speculative metadata. Core-function text is not a serializer
for speculative versions or cost models.

## Datalog and CCDB contracts

Declare integer/symbol columns and mark derived relations explicitly. Rules use
`Term::var`, `Term::constant`, and `Term::any`. Every head or negated variable must
occur in a positive body atom. Anonymous wildcards in negation are existential
tests. Repeated variables enforce equality. Type/arity errors, unbound variables,
and recursion through negation are rejected. Extensional facts cannot be written
to derived relations. Relations have set semantics and deterministic tuple order.

Evaluation restricts work to the queried dependency closure, uses bound-column
indexes for joins, and computes each stratum's least fixed point. The default
semi-naive evaluator seeds nonrecursive rules once, then evaluates recursive rules
with at least one previous-round delta input. Nonlinear and mutually recursive
rules are supported; negative inputs come only from completed lower strata.
`Program::evaluate(..., limits, datalog::Evaluation::Naive)` retains a synchronous
full-recomputation reference for differential tests. `Result::statistics` records
rounds, rule evaluations, tuple visits and index builds. These are deterministic
engineering counters, not measured wall-clock speedups. Fact and work
budgets throw `datalog::BudgetExceeded` on exhaustion. Fact validation and index
construction consume work too; joins are capped at 256 body atoms to bound native
recursion depth. Malformed public term/type discriminants are rejected. This is
a function-free engine with a C++ rule API; the dossier's illustrative `.decl`
syntax is not a parser grammar.

CCDB starts at an empty bootstrap revision. A snapshot owns its data and remains
usable after later commits or database destruction. Transactions stage a private
copy, run validators before publication, and atomically compare the base snapshot
identity. Conflicting or foreign revisions cannot publish. Failed transactions
can be corrected or aborted. Destroying an uncommitted transaction discards it.
Allocated/reserved IDs are never recycled, including after abort.

Caches record transitive **base-relation** dependencies, including negative
dependencies. Unrelated commits retain cached results; affected queries recompute
from primary facts, so deletions retract recursive consequences correctly. The
invalidation granularity is a relation, not individual tuples. By default,
transactions use whole-revision conflict checks. Facts are copied, rather than
maintained by a disk-backed storage engine.
Snapshot reads and independent transactions are thread-safe; each transaction
itself is single-threaded. The audit log records successful commits and their
changed relations. Rejections propagate exceptions to the caller.

### Dependency-checked independent transactions

`commit()` retains its strict stale-revision rejection. For independently edited
relations, opt into `commit_disjoint()`. Transaction `relation()` and `query()`
reads automatically register base-relation dependencies, including transitive and
negative query inputs. Writes register their relation too. `preview()` exposes an
unrestricted snapshot, so it conservatively records every primary relation as read.

```cpp
auto first = database.begin();
auto second = database.begin();
// Read and edit distinct relations through these transaction objects.
first.insert("Profile", {std::int64_t{42}});
second.insert("TargetFlags", {std::string("scalar")});
first.commit_disjoint("profile update");
second.commit_disjoint("independent target update");
```

The example assumes those relations were declared in the database's program.
Rebasing compares per-relation **versions**, not just contents, so an intervening
change followed by restoration (ABA) still conflicts. The engine overlays only
the write set on the current head, re-runs all validators against that merged
candidate, and publishes with a head-identity check. Concurrent head changes
trigger bounded retries; conflicting read/write sets always reject. Validators
must be replay-safe, depend on their supplied snapshot, and avoid external effects.

For facts consumed through an external base snapshot, call
`transaction.depend(base, {"AnalysisRelation", ...})` before `commit_disjoint()`.
The base must be the transaction's exact source snapshot. Unregistered external
reads are outside this opt-in protocol's correctness contract; use ordinary
`commit()` when dependencies cannot be enumerated. Whole-function IR publication
continues using the conservative strict path. Audit events record the original
source revision, actual parent, changed relations and registered reads.

### Checkpoints

`Database::checkpoint()` captures primary facts, the allocation high-water mark,
the originating revision, and a complete canonical schema/**rule** identity.
`Database::restore(expected, text, limits)` parses under byte/fact caps and
publishes a new local revision through the usual validators and conflict check.
It rejects truncated, trailing, duplicate, mistyped, or incompatible data; cached
analyses are never deserialized. Old snapshots remain usable after restoration.
Allocated-but-aborted IDs remain reserved across a checkpoint round trip.

```cpp
auto checkpoint = database.checkpoint();
ccdb::Database restored(ir::program(), [](const auto& s) { ir::verify(s); });
auto revision = restored.restore(restored.snapshot(), checkpoint);
```

Checkpoint strings preserve symbols containing quotes, backslashes, newlines or
NUL bytes. The caller handles file I/O and atomic file replacement. Checkpoints
are not authenticated artifacts, write-ahead logs, or serializers for process-local
assumption registries.

## Query-based optimization

`Fold`, `ConstCandidate`, `CopyCandidate`, `CopyValue`, and `Dead` are Datalog
relations. They drive constant folding for add/sub/mul/eq/ult; add-zero, sub-zero,
sub-self, mul-zero, mul-one, eq-self, ult-self; constant/equal-arm select reduction;
copy propagation; and dead pure instruction elimination. Rule priority and tuple
order resolve conflicts deterministically. Every replacement is reverified and
committed before the next query. Explanations retain consumed relations and local
rewrite revision numbers alongside the original source revision.

`query::Candidate` construction is restricted to the built-in engine or an
unchanged verified baseline. CostCC can rank these objects but cannot manufacture
a rewrite or bypass its legality checks. A candidate's semantic root is an actual
snapshot identity, not merely a revision number. Speculative snapshots require
recovery-aware transformations and are rejected by the generic optimizer.

`query::Options` bounds rewrite count, cumulative Datalog work, per-query facts,
and cumulative conditional-analysis work. Exhaustion returns the last verified candidate prefix (possibly the
unchanged input), with `budget_exhausted()`, `stop_reason()`, and conservative
charged `query_work()` / `analysis_work()` metadata. Partial edits are never exposed, and running
out of optimization budget does not itself fail compilation. These are work-unit
limits, not wall-clock deadlines; structural verification has separate cost.

The second optimization regiment also includes:

- **Branch folding:** constant/nonzero conditions and identical targets become
  jumps; unreachable blocks are removed and surviving phi edge lists repaired
  in the same verified transaction.
- **Uniform phis:** identical incoming values reduce to their common value.
- **Local CSE:** pure expressions are value-numbered inside one block, with
  commutative keys for add/mul/eq. Phis are not ordinary expression keys.
- **Load forwarding:** a previous successful load/store of the same address
  proves availability and bounds. An intervening unknown-alias store destroys
  the evidence; stores at provably different constant addresses retain it.
  Analysis never forwards across blocks or beyond this dialect's fixed-size,
  single-threaded, nonvolatile memory model.

### Third-regiment global optimization

`query::propagate(function, work_budget)` implements sparse conditional constant
propagation (SCCP) with the lattice `Unknown -> Constant -> Overdefined`, sparse
SSA-user updates and an executable-edge worklist. Phis meet only executable
incoming edges. A late edge or a changing backedge widens constants; no edit is
authorized by a provisional analysis value. Loads and parameters are overdefined,
and the analysis conservatively follows control flow beyond potentially trapping
accesses rather than assuming they succeed. Budget exhaustion throws instead of
returning a partial fixed point. `Propagation::explain()` exposes values and edges.

After cheap relational/local rewrites, the optimizer uses completed SCCP constants
to replace pure expressions and phis, then revisits branch folding and CFG repair.
Promoted phis move after the remaining parallel phi group. Set
`Options::conditional_constants = false` to disable this exploration; its work is
bounded separately by `Options::analysis_work`.

`Needed` / `DeadGroup` relations additionally trace dependencies backwards from
observable roots, including trapping loads, stores, control flow, returns and
recovery bindings. Entire unneeded pure cycles are removed in one verified
transaction: deleting one phi at a time would leave dangling cyclic uses. This
does not remove CFG loops or change observable divergence. Rooted DCE is
conservative about all potentially trapping memory operations.

### Bounded semantic validation

`query::validate(before, after, inputs, step_budget)` compares return values,
traps, final memory, and ordered write traces. It reports `Passed`, `Mismatch`
(with the first reproducible counterexample), or `Inconclusive` when either
execution exhausts its resource budget. Two equally timed-out executions never
count as equivalent. Empty or wrong-arity input domains are rejected.

`query::corpus(arity, seed, samples, max_memory)` creates reproducible boundary
and random cases under allocation caps. `apply_checked` requires a passing report
before calling the ordinary stale-revision-checked publication path. This gate
supplements, rather than replaces, built-in rewrite legality.

```cpp
auto inputs = query::corpus(ir::load(source).arity, 728);
auto report = query::validate(ir::load(source), ir::load(candidate.result()), inputs);
// inspect report.explain(), including its finite-domain scope
auto committed = query::apply_checked(database, candidate, inputs);
```

The CLI `validate BEFORE AFTER [SEED]` exposes the same finite-domain check (exit
0 passed, 1 mismatch, 2 inconclusive); `run FILE [I32...]` executes with empty
initial memory. No finite-domain result is a universal proof.

On a mismatch, the CLI also prints a reduced reproduction. The library API
`query::minimize(before, after, input, ReductionOptions)` first rechecks the input,
then removes memory suffixes and simplifies argument and memory-cell bit patterns.
Surviving cells keep their addresses. Each accepted reduction must still produce
a completed mismatch in return value, trap, memory, or ordered writes. A trial
that exhausts its execution budget is counted as inconclusive and discarded.
The mismatch category may change during reduction.

`ReductionOptions::evaluations` caps paired executions (default 256, including
the required initial check); `step_budget` caps each execution (default 100000).
Exhaustion returns the last reproduced witness with `budget_exhausted = true`.
An initially matching or inconclusive input, zero evaluation budget, or wrong
argument count is rejected. `Reduction::explain()` includes the input, outcomes,
work counters, and stopping status. Reduction is deterministic and greedy; it
does not claim a globally minimal input or minimize the programs themselves.

## Speculation and recovery

```cpp
auto assumptions = std::make_shared<speculative::Assumptions>();
auto assumption = assumptions->assume("argument usually equals 42");
auto version = speculative::Version::specialize(source, assumptions,
                                               {{x, 42, assumption}});
auto hit = version.execute({42});
auto miss = version.execute({7}); // resumes baseline at the failed guard
assumptions->invalidate(assumption);
auto invalidated = version.execute({42}); // baseline recovery
```

The supported refinement is a parameter instruction replaced by a constant, with
a guard immediately before that instruction checking the actual argument and
assumption epoch. `Assumption`, `Guard`, `Frame`, and `Binding` are stored CCDB
relations. Guards and frames have version-local IDs; assumptions have
registry-local IDs. Each frame records the baseline block/offset and the SSA values
live immediately before its parameter. Liveness comes from the baseline body and
includes future phi-edge uses; dead preceding temporaries need no binding.
Structural refinement verification independently recomputes these requirements,
checks that every required value is present, and rejects unavailable bindings.
Older conservative frames containing additional available values remain valid.
Liveness uses the standard Datalog analysis limits; failure to finish analysis
rejects specialization before publication.

Execute a speculative artifact through `Version::execute`, which interprets its
guard metadata. The core `ir::load` projection alone represents only its body.
On a failed guard, recovery retains memory, write trace, and consumed step budget,
reconstructs locals, and resumes the baseline **at** the original parameter. It
does not restart the function or replay prior effects. Every guard can be forced
to fail by ID for testing. Immutable versions pin their baseline snapshots.
Unknown forced-guard IDs are rejected. `Version::statistics()` exposes atomic
counts of executions, deoptimizations, predicate failures, invalidations and
forced failures. Version copies share counters. Multi-counter reads during
execution are not coherent epoch snapshots; quiescent totals are suitable for
profitability evaluation.

Invalidation is synchronized with execution: it waits for active executions to
leave their shared epoch lock; future entries fail the invalidated guard. This is
a coarse safepoint policy rather than asynchronous machine-code patching. A new
assumption gets a new identity. The SSA evaluator has one frame and no operand
stack, so nested frames, object materialization, native stack maps, and OSR are
outside this dialect.

## Learned cost ranking

`cost::Model::train` fits normalized ridge regression on four features: pure-op,
load, store, and control-transfer counts. Training and calibration workload names
must be disjoint. Context keys include target, ISA, cache description, optimization
context, and profile epoch; model versions are explicit caller-supplied strings.
Samples must have finite nonnegative features and positive measured latency.

The calibration score is `1 / (1 + RMS relative error)` on held-out samples. It is
an empirical ranking confidence score, not a statistical correctness guarantee.
Predictions also carry an empirical latency envelope using the largest absolute
held-out residual: `[max(0, prediction - error), prediction + error]`. Calibration
evidence records both sample counts and distinct workload counts for training and
calibration, so repeated observations are not presented as distinct workloads.
These ranges describe observed error; they are not statistical confidence
intervals and give no coverage guarantee for future measurements.

Unseen contexts, feature values outside training ranges, invalid predictions, or
confidence below the threshold trigger a deterministic weighted-count heuristic
for the entire candidate set. Missing calibration evidence also triggers fallback.
For multiple candidates, the predicted winner's upper envelope must be strictly
below every alternative's lower envelope. Overlap or a tie falls back to the
heuristic, even with a high aggregate confidence score. No shipped model claims
hardware speedups.

`Ranker::choose` logs context, model version, source revision, candidate features,
predictions, confidence, calibration counts, error envelopes, selected candidate,
and selection reason. `observe`
attaches measured latency and flags excessive relative prediction error. Drift
quarantines the active model after a model-selected decision; subsequent choices
use the heuristic even for in-range targets. `replace_model` requires a fresh
version identity before restoring model use. Feedback for retired models cannot
quarantine a replacement. No feedback silently retrains models or changes legality.

`Model::serialize` / `Model::parse` preserve version, context, full-precision
coefficients, feature ranges, calibration confidence and evidence in a bounded, versioned,
locale-independent format. Invalid ranges, non-finite coefficients and truncated
or trailing artifacts are rejected. Format version 2 adds calibration evidence;
version 1 remains readable, but its missing evidence makes ranking fall back.
Re-serializing an older model preserves the absence of evidence. Persisted models contain no authority to
create or publish unchecked candidates.

### Dossier traceability for these extensions

Paths below are relative to the source-of-truth directory named above. The
dossier describes proposed architectures; these implementations cover the
following bounded portions of those proposals.

| Source | Implemented contract | Regression suite |
| --- | --- | --- |
| `chapters/04-deoptimization-as-an-ir-operation.md`, sections 1, 3–4 | Live recovery bindings, independent completeness/availability checks, preserved effects and resume budget | `novostone-recovery` |
| `chapters/11-compiler-with-a-learned-cost-model.md`; `chapters/27-compiler-with-explicit-uncertainty.md`, sections 1–4 | Held-out residual evidence, workload counts, uncertainty-aware ranking and conservative fallback | `novostone-uncertainty` |
| `chapters/39-semantic-fingerprints-and-metamorphic-compiler-testing.md`, section 3 | Reproducible input reduction using outputs, traps and effect traces under explicit budgets | `novostone-reduction` |

## Verification scope and current boundaries

CTest covers recursive/negated analysis, graph differential tests, cache
invalidation after deletion, concurrent publication, SSA rejection cases, text
round-trips, arithmetic overflow, memory traps, seeded random optimizer
differential tests, forced failure at every guard, exactly-once effects through
deoptimization, concurrent invalidation, calibration, and model fallback.
The added regression executable covers loop sums against an independent reference,
parallel cyclic phis, mid-phi suspension, malformed incoming edges, CFG/phi repair,
thousands of aliased/out-of-bounds memory cases, effect-dropping mutants, bounded
validation rejection, checkpoint recovery, model quarantine and concurrent cache
population during transaction validation.
The third test regiment compares semi-naive and naive recursion, relational and
independent worklist liveness, and abstract constants against observed executions.
It covers nonlinear/mutual recursion, cyclic deletion, dependency rebasing, write
skew, ABA and negative reads, replayed validators, executable-edge widening,
generated effectful CFGs, dead phi groups, and irreducible/nested loop boundaries.
The dossier suites cover compact and empty recovery frames, future phi-edge uses,
forced failure with prior stores, uncertainty overlap despite high confidence,
model-format migration, malformed calibration evidence, deterministic witness
reduction, and inconclusive or exhausted reduction trials.

This implements the dossier's executable reference and bounded vertical-slice
approach. Verification is structural plus semantic differential testing, not an
SMT proof or machine-code translation validation. The current dialect excludes
calls, floating point, allocation, language-level exceptions, atomics,
FFI, object shapes, and native code generation. CCDB is in-memory; Datalog uses
semi-naive fixed-point recomputation of affected queries rather than persistent
differential dataflow across revisions.
Future extensions can retain the same dependency and recovery contracts.
