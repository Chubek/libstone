# Prolific v1 design

Prolific records observations, not proofs of program correctness. A host adapter
maps compiler passes, IR identities, VM dispatch, allocation and faults into a
language-neutral event stream. No unknown instruction is interpreted as safe.

## Contracts and invariants

- C++20 recorder and versioned C11 embedding ABI; copied strings and source/IR
  identities, monotonic timestamps, serialized sequence numbers, per-thread
  nested spans. Ends must match the creating thread's innermost span. Thread
  IDs identify recorder threads, not OS thread IDs. Events never migrate spans.
- Admission is bounded by event, thread, nesting and string limits. Rejected or
  exhausted recording is counted and makes the snapshot incomplete. Unclosed
  spans are reported, never silently charged as completed work. Recording and
  snapshots can run concurrently; destruction requires quiescent producers.
- JSON schema `moonstone.prolific/1` contains raw events, inclusive/exclusive
  timing summaries, error observations, script findings and collection loss.
  Findings are observational/heuristic, never independently verified repairs.
- LMDB transactions through the actual vendored lmdbxx wrapper atomically
  persist a versioned, bounds-checked binary snapshot and its JSON report.
  Run IDs cannot overwrite existing data without explicit replacement. Failed
  writes abort; map exhaustion is an error. Offline replay uses cached events.
- Vendored Lua and Kaguya back `require('lprofcc')` and `require('lprofvm')`.
  A script registers event callbacks and produces bounded findings. Replay is
  deterministic in event order, runs outside recorder locks, and is explicit,
  not on producer threads. Script errors/exhaustion fail analysis, not recording.
  Scripts have limited libraries and a best-effort instruction budget, but are
  **trusted native-process code, not sandboxed**; use process isolation otherwise.
- Linux injection supports `LD_PRELOAD` on GCC/Clang `-finstrument-functions`
  hosts; generic compiler/interpreter integrations use the C ABI. Uninstrumented
  binaries, async signal handlers, fork-without-exec, JIT rewriting, sampling,
  symbolization and crash-safe finalization are outside v1's supported scope.
  Native addresses are retained as identities, not fabricated source spans.

## Verification plan

Exercise nested timings, counters, errors, original locations, multithreaded
recording, mismatched/cross-thread ends, every admission limit, JSON escaping,
binary corruption and allocation bounds, durable close/reopen/replay, duplicate
run IDs, transaction failure on a full LMDB map, Lua callbacks/filtering/errors/
instruction exhaustion, plain-C embedding, and an actually preloaded instrumented
host. Preserve the existing Moonstone suite and compile public headers alone.

This is an instrumentation-based profiling baseline, not a universal binary
instrumenter, optimizer, memory-safety checker, or production security boundary.

## Refinement: admission and serialization

Reject over-budget event views before copying or scanning their payloads. Only
admitted inputs may allocate event strings; rejection must still increment loss
without changing span state. Enforce the wire budget before every append,
including metadata and trailing counts, rather than after allocating a record.
Reject contradictory reports containing findings from failed/unrequested
analysis. Regressions cover oversized admission, recovery after rejection and
invalid analysis/findings combinations; rerun native and sanitizer suites.
