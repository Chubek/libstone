# Loom, Conduit and Chorus runtime contracts

## WorkStealingPool

`WorkStealingPool` creates N worker threads and per-worker deques. A worker
pops from its own tail and steals from other queues' heads. The pool uses
mutex-protected queues and an atomic queued count for condition-variable wakeup;
admission and the sleep predicate share a mutex to prevent lost notifications.
`submit` returns a `std::future`, propagating worker exceptions through the
future. `stop()` rejects new work and drains already accepted tasks before
joining workers. Concurrent `submit`, `request_stop` and `stop` are supported;
concurrent joins are serialized. `stop()` throws `logic_error` in a pool worker;
use nonjoining `request_stop()` there. Destruction is **not** legal from a pool
worker or concurrently with any API call. Tasks must finish or observe their
cancellation token; there is no forceful thread interruption.

`WorkStealingPool(workers, queue_limit=0)` optionally limits queued tasks,
excluding running callbacks; zero preserves unlimited admission. A stopped or
saturated pool throws `runtime_error` from `submit` without invoking the rejected
task. Submission acquires mutexes and allocates, but does not wait for queue
space. Futures own results; tasks may start on any worker, but never migrate
during a callback. Stop and scope join block the calling OS thread.

`TaskScope` owns submitted void tasks; a failing child immediately requests
cooperative cancellation of siblings, without waiting for join-order observation.
`join()` drains all children and rethrows the first exception it observes.
Tracking storage is acquired before task admission, including failure paths.
The destructor requests cancellation then waits, suppressing exceptions.
A single-thread pool can deadlock if a task blocks waiting on another task
queued to the same pool. Use a blocking pool or a continuation-based awaiter.

`CooperativeScheduler` runs FIFO callbacks on one caller thread. `post`
inside a callback is permitted. Recursive `run` is rejected. Its budget
limits how many callbacks execute per call.

## Channel and select

`Channel<T>(capacity)` with capacity >0 is bounded FIFO. Capacity 0 is a
synchronous rendezvous: `send` returns only after a receiver consumes the
value. `close()` rejects new messages and permits committed data to drain.
Every rendezvous send waits for its own offer's acknowledgment, not a shared
empty-slot predicate that can be invalidated by a later sender.
`try_send` on a zero-capacity channel can enqueue when a receiver is waiting;
unlike blocking `send`, it does not wait for acknowledgment. Blocking calls
must not be used in a single-thread event loop or scheduler worker.
Messages require noexcept move construction and destruction (including actors);
throwing moves are rejected at compile time. Methods take messages by value:
rejection does not restore a caller's moved-from value. Channel state and
message publication are protected by its mutex; successful receives synchronize
with publication. Close is not cancellation: a published rendezvous send still
waits for a receiver to drain its offer. Channels must outlive all operations.
Message moves/destructors execute under channel or mailbox locks and must not
reenter that same object. Actor handlers themselves run outside the mailbox lock.

`select_receive` supports only buffered channels. It uses a generation-based
wakeup signal and rechecks readiness, so notifications occurring during the
readiness scan are not lost. It rotates case priority between notification
rounds; this is not Go's randomized fair selection or atomic mixed send/recv
select. It throws on zero-capacity channels rather than claim incorrect
rendezvous semantics. Readiness and closed-and-empty are sampled under a single
channel lock, so a close race cannot hide a queued value. Duplicate channel
cases are supported. Deadline arithmetic saturates, nonpositive timeouts perform
one scan, and notifications cannot extend the deadline indefinitely.

`select_receive(channels, timeout, rotation=0, std::stop_token={})` blocks the
calling OS thread. Timeout returns `nullopt`; cancellation throws
`SelectCancelled`. Cancellation is checked before each readiness scan and cannot
undo a completed receive. Closed-and-drained channels return a choice with
`closed=true`. Select is not an atomic transaction across cases or channels.
It never migrates the calling thread or cancels other channel operations.

Selection observers are RAII registrations removed on success, timeout,
cancellation and exception. `observe(signal)` exposes this move-only registration;
its channel must outlive it. `observer_count()` samples retained registrations
under the channel mutex for diagnostics. The old `watch(signal)` remains a
weak, lazily pruned subscription; prefer `observe` for bounded lifetime. Never
hold a `WakeSignal::mutex` while calling a channel method (lock order is channel
then wake signal).

## Actor

`chorus::Actor<T>` serializes handling of a FIFO mailbox on a pool. Accepted
messages run in submission order. Handler exceptions close the actor and
propagate from `close_and_wait()`. The creator must keep the scheduler alive
until the actor drains. Self `close_and_wait()` throws `logic_error`; handlers
can use nonjoining `close()` to reject new sends and drain accepted messages.
Other blocking waits from a saturated pool can still deadlock.
`create(pool, handler, mailbox_limit=0)` bounds queued entries, excluding the
active handler; zero is unlimited. `send` returns false on saturation or close,
and propagates pool admission errors with the mailbox/scheduled flag rolled
back. It returns acceptance, not delivery acknowledgment. Handler failure
discards queued messages and is rethrown by every `close_and_wait()`.
Send and close acquire mutexes but do not wait for handler completion;
close-and-wait blocks the calling OS thread. Scheduled work retains the actor
until draining ends. Different drains may use different workers, never
concurrent handlers. No supervision/restart or forceful cancellation exists.

## C++ 0.2 compatibility tightening

Existing ordinary calls retain their behavior and the C runtime ABI remains v1.
`WorkStealingPool::stop` is no longer `noexcept`, to reject self-join rather than
terminate. Channel/actor types with potentially throwing moves or destructors
are deliberately no longer supported; wrap such payloads in noexcept-movable
owning handles. The select template gains an optional stop-token parameter;
code taking its address must account for the additional parameter.
See [the P0 design](06-runtime-hardening.md) for failure and ownership invariants.
