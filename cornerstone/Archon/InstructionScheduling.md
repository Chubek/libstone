# Instruction scheduling

## Native backend

The native backend defaults to verified MachineIR source order. To opt in to
list scheduling, pass `ARCHON_BACKEND_SCHEDULE_PURE` to
`archon_backend_compile_ex` or `archon_backend_compile_text_ex`, or run
`archon-backend --input PATH --schedule-pure`. Register allocation and emission
both consume the resulting order.

Scheduling is local to each block and can move only explicit pure, total
operations: constants, copies, wrapping arithmetic, total division and
remainder, bitwise operations, masked shifts, and integer comparisons. Every
other opcode, including all width and pointer forms of loads and stores,
checked arithmetic, pointer/integer casts, conversions, calls, and address
formation, is pinned at its original index. No instruction crosses
a pinned operation or a block terminator, and block parameters and edges are
unchanged.

Within each contiguous region of movable instructions, higher priority goes
to longer local def-use chains. With
`archon_backend_compile_text_with_schedule_hints`, the native RV64 scheduler
can prefer a caller-marked SSA destination at equal depth. Remaining ties
are broken by original index. Hints are local to the exact input and cannot
alter barriers or def-use constraints. Each
producer ranks strictly ahead of its consumers, so stable buckets can pick
the highest-priority ready instruction without a search loop. Each region
uses linear time and is bounded by the native SSA and instruction limits.
The priority is a count of dependency-chain nodes, not a cycle estimate.
The native path does not run the modulo solver, model hardware latency or
resources, or promise an improvement in runtime or register pressure.

Before instructions are copied for lowering, a separate check verifies that
the index plan is a permutation within the original regions, that pinned
instructions did not move, and that each local SSA definition still precedes
its uses. Instructions are copied unchanged from those verified indices; a
failed check does not publish a schedule.

## Generated `ISched.c`

The legacy standalone modulo scheduler and linked `.i` pipeline use the
`ISched.h` API. The standalone scheduler uses the supplied model; its
ArchDB-derived unit-capacity model is conservative, not target timing data.
The linked pipeline keeps its documented conservative ordering barriers and
artifact provenance; a SAT witness for those internal barriers does not
establish any hardware timing property.

| Result | Meaning |
| --- | --- |
| `ISCHED_STATUS_SAT` | The solver found a verified witness in the bounded model. |
| `ISCHED_STATUS_FALLBACK` | Search was inconclusive, but the deterministic serial fallback produced a verified witness within `max_ii`. |
| `ISCHED_STATUS_UNSAT` | A lower bound ruled out the search range, or the solver ruled out every remaining bounded formula. This is not an unbounded impossibility claim. |
| `ISCHED_STATUS_UNKNOWN` | Search was inconclusive and the serial fallback did not produce a witness within the bounds. For example, simultaneous zero-latency constraints need not admit a serial fallback. |
| `ISCHED_STATUS_VERIFY_FAILED` | An internal result failed verification. No usable schedule is returned. |

The SAT horizon is `II * max_stage`. The fallback only places operations
in stage zero and may increase II without exceeding `max_ii`. A zero
`search_limit` skips SAT construction and search. Search and construction
limits are not proofs of infeasibility. The pressure ranking considers only
examined candidates, not necessarily every feasible schedule.

Before a successful result reaches the linked pipeline or is written by
the standalone scheduler, `isched_verify` checks its operation permutation,
canonical cycle/ID order, cycle/slot/stage consistency, dependency
inequalities, periodic resource capacity, and pressure summary. For
annotation-assisted runs, verification uses the effective model, including
accepted latency and capacity overrides. Temporary priority ranks are not
published as operation IDs. Verification checks feasibility of the result,
not the history or optimality of the solver, and does not cover any later
external transformation of a serialized artifact.
