# Parallax and Sentinel

## Parallax

`parallel_for` partitions an explicit iteration range into finite chunks,
submits tasks and waits for them. It is the caller's responsibility to prove
iteration independence; the function performs no automatic dependence check.

`parallel_reduce` requires the supplied combination to be associative with
an identity. Arithmetic reduction reassociation can change floating point
results, exceptions and overflow behavior. The current algorithm retains
chunk order in the final fold but does not preserve per-element sequential
operation order.

Both algorithms join admitted jobs before returning, including submission,
capture/allocation and worker failures. In particular, a bounded pool rejecting
later reduction jobs cannot leave earlier jobs running with references to
expired caller data. The first reduction failure is rethrown after draining.
These calls block the calling OS thread and must not be nested on a saturated
pool. Failure may leave partially computed output; no transactional rollback is
provided.

`lane_map` implements portable scalar lane grouping; it emits no SIMD-specific
instructions. Build a future vectorizer separately using Firestone libintrin,
Sandstone target details, pack selection, legality and cost modeling.

`loop_dependence` compares simple affine byte accesses from explicitly known
allocations across pairs of iterations. If its pair budget is exceeded or
address arithmetic is outside signed range, it returns `Unknown`. More complex
aliasing, unknown calls, volatile and atomic ordering, exceptions and external
effects are **not** represented and must block automatic parallelization.

## Sentinel

`VectorClock` tracks logical events. The caller must add synchronization edges
using `join`, then advance clocks on events. `races` compares an externally
supplied trace and reports unordered conflicting accesses where at least one
is non-atomic. It is not a dynamically instrumenting race detector and is
not sufficient for whole-program race-freedom claims.

`LockOrder` checks whether adding held->acquired creates a cycle in the
observed lock-order graph. It identifies a possible cyclic locking discipline,
not a proof of reachable deadlock. Validate thread and lock identities at
host-language integration boundaries.

`integration/Moonstone.hpp` converts race records into common Moonstone
`Diagnostic` values and checks declared task-capture suspension/migration
capabilities. The latter checks annotations only: Chisel must establish those
annotations from actual types and ownership semantics before they can be
used as trusted evidence.
