# Profile-driven speculation and SpecIR v1

## Input contract

Reuse the bounded Recorder, event sequence/source identities, LMDB snapshots and
offline replay. `observe` emits a reserved versioned Instant event with a
length-delimited profile payload; existing v1 caches remain readable. Categorical
profiles: type, branch, call, interoperation, system interaction, lowering,
exception and custom. Numeric profiles: value, computational cost and allocation.
Each observation identifies domain, site, context (build/target/workload identity),
dimension (operand/metric/contract being measured), outcome or integer/unit, and
original location. Different contexts, units, dimensions or locations never merge.
Unknown profile versions and malformed reserved events fail speculation rather
than silently disappearing. Ordinary non-profile events remain ordinary events.

## Output and invariants

`speculate` computes bounded histograms and exact evidence counts. Numeric
profiles also carry min/max/sum with explicit overflow. Candidate selection uses
minimum sample count and integer dominance thresholds; ratios describe observed
frequency, never probability guarantees or proofs. Deterministic ordering and
tie handling make replay reproducible. Incomplete collection, small samples and
truncated histograms produce warnings, not speculative optimization advice.

The output is executable Lua *data construction*: `require('lspecir')`, followed
by a returned versioned module of `profile`, `guard`, `advice` and `warning`
instructions. Every advice references a guard and evidence profile. Guards name
an explicit predicate, expected value/range, evaluation phase and miss behavior.
External effects and costs require post-observation monitoring, never speculative
elision/reordering of effects. Lowering suggestions require target-context and
independent legality checks. No generated guard is installed or claimed proved.

`lspecir` is an installable pure Lua library. Its constructors and module validator
check schema, IDs, references and guard/advice relationships before dispatch.
`lspecir.translator(handlers):translate(module, context)` preflights every required
handler, then invokes user handlers in IR order. No implicit compiler, execution
of predicates or ignored unsupported instructions. Handler failures propagate;
external effects performed by a user handler cannot be rolled back by the library.

Speculation uses bounded profile/bucket/instruction/output budgets. Exhaustion
of global budgets throws, yielding no partial module; bucket exhaustion is
explicit per-profile evidence loss. Lua quoting must preserve UTF-8, control
bytes and exact 64-bit decimal data without permitting code injection.

## Verification

All categories, thresholds/ties, numeric extrema, context isolation, truncated
histograms, invalid/future records, incomplete traces, global budget exhaustion,
durable LMDB replay, exact Lua literals and source identities; load generated Lua
with the actual vendored VM and translate through lspecir with user handlers.
Reject malformed IR and missing handlers before dispatch, and propagate callback
failures. Exercise CLI generation and installed module discovery. Preserve the
existing suites, run ASan/UBSan and public-header checks.
