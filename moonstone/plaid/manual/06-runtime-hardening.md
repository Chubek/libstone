# P0 runtime hardening design

## Scope and invariants

This change hardens the existing blocking runtime; it does not add fibers,
async channel operations, atomic rendezvous select, or a race-freedom proof.
Firestone's `libconcurrency/Channel.hpp` uses noexcept-movable messages but
capacity zero means *unbounded*, unlike Plaid's rendezvous. It cannot replace
Plaid's channel without changing semantics. No vendor integration is claimed.

- Pool admission and the condition-variable sleep predicate share a mutex.
  Accepted tasks transition queued -> running -> completed/failed exactly once.
  Queue mutexes publish captures; futures publish results and exceptions.
  Stop transitions accepting -> draining -> joined. Joining is serialized;
  no admission is permitted after the draining transition.
- `request_stop()` may run in a worker; it only closes admission and wakes
  sleepers. `stop()` joins and rejects worker self-join with `logic_error`.
  Destruction still requires an external owner and no concurrent API calls.
- An optional pool queue limit bounds queued (not executing) tasks. Admission
  fails with `runtime_error` without running the rejected callable. The default
  remains unlimited. TaskScope reserves child storage *before* admission.
- A failing scope child requests shared cancellation before publishing its
  exception. Waiting for join-order observation can deadlock an earlier child
  that is waiting for cancellation. Cancellation remains advisory, not an
  interrupt; join drains all children and rethrows the first observed failure.
- Parallel reductions must join all admitted jobs even if later submission,
  capture copying, allocation or result initialization throws. A future from
  this executor does not implicitly join on destruction. Bounded admission must
  not let jobs using caller-owned references escape a failed parallel call.
- Each rendezvous offer has its own acknowledgment, protected by the channel
  mutex. A sender never waits for a subsequent sender's offer. Close preserves
  published offers for draining. Messages require noexcept move construction
  and destruction, so extraction cannot strand waiters after a partial move.
- Select registrations are removed on every exit, including validation errors,
  timeout, cancellation and exceptions. Readiness and closed-and-empty are
  observed atomically. Observer notification allocates no memory after commit.
- Buffered receive select accepts an optional `std::stop_token`; cancellation
  throws `SelectCancelled`, distinct from timeout (`nullopt`). Cancellation is
  checked before each scan, not retroactively after consuming a value. It does
  not cancel channel sends or close channels. Duplicate cases remain supported;
  rendezvous cases remain rejected. Very large timeouts must not overflow.
- Actors retain themselves while scheduled, serialize message handling, and
  optionally bound queued mailbox entries (excluding the active handler).
  Rejected sends return false. Nonblocking `close()` stops admission but drains
  accepted messages. Self `close_and_wait()` throws rather than deadlocking.

## Blocking, migration and ownership

Pool submission, request-stop, actor send/close and observer registration never
wait for task completion, but acquire mutexes and may allocate. Pool stop,
TaskScope join/destruction, channel send/receive, select, and actor
close-and-wait block the calling OS thread. They must not wait for work queued
behind themselves on a saturated executor. Tasks may be assigned to any worker
but do not migrate during execution. Actors may use different workers between
drains, never concurrent handlers. Channel calls run on the caller's thread.

Accepted work owns its captures until completion; rejected work is destroyed
without invocation. Channel and actor message arguments are passed by value:
even rejection may consume the caller's moved-from argument. Channels must
outlive all operations and registrations; the pool must outlive its actors and
scopes. Cancellation is cooperative and never interrupts running callbacks.
TaskScope remains single-owner, not concurrently callable.

## Validation plan

Keep the original tests. Add bounded-time CTest regressions for idle wakeups,
concurrent producer/stop/join, worker exceptions and self-stop, queue and mailbox
limits, canceled/duplicate/invalid selects, close-and-drain, move-only messages,
MPMC rendezvous acknowledgment, actor self-wait, failure and retained lifetime.
Exercise reduction admission failure with a blocked worker and compare explicit
parallel algorithms against scalar loops across empty, uneven and small ranges.
Use fixed seeds and yields to vary schedules. Compile public headers separately
with GCC/Clang and the ABI with C11; run ASan/UBSan and attempt TSan. Record
environmental sanitizer failures honestly. These checks are not a production
audit or proof of race freedom.

## Local validation record

Linux x86-64, standalone Moonstone, 2026-11-12:

- GCC 16.2.1 Debug: all 20 CTest tests passed.
- Clang 22.1.8 Debug: all 20 tests passed.
- Clang ASan/UBSan: all 20 tests passed, no sanitizer reports.
- GCC TSan: all 20 tests passed, no sanitizer reports. Each of the three
  lifecycle stress tests also passed 30 consecutive repetitions.
- All 16 public Plaid headers compiled independently with warnings as errors
  under GCC and Clang; the ABI consumer compiled as strict C11. Both compilers
  rejected channel/actor instantiations with throwing-move messages.
- Clang static analysis of `examples/runtime_costs.cpp` reported no findings;
  this limited check is not a whole-runtime analysis. Changed C++ files pass
  clang-format validation. Actor/parallel examples returned 15 and 499500.

An illustrative GCC `-O2` run of `examples/runtime_costs.cpp` (10,000 operations)
reported: direct atomic 42 us, two-worker pool batch 9,464 us, sequential pool
round trips 41,847 us, rendezvous transfers 38,655 us, capacity-64 transfers
2,106 us, and empty select 1,238 us with **zero retained observers**. This
single noisy run includes scheduling and allocation costs. Pool construction is
excluded; channel measurements include receiver thread startup/join. It
demonstrates overhead and cleanup rather than a performance guarantee.

Not validated here: root LibStone build, Windows/BSD/MSVC, vendor adapters,
formal scheduler proofs or production deployment. Existing warnings outside
Plaid remain; only Plaid's isolated headers are required warning-clean by this
change. The v1 single-thread C ABI and all P1–P5 limitations remain unchanged.
