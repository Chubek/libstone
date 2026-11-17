# Concurrency status

No concurrent task scheduler, promise, channel, actor, synchronization primitive,
parallel worker, deadline, cancellation or libuv adapter is implemented.
Synchronous generator suspension is implemented (CONTINUATIONS.md); it must not
be advertised as asynchronous execution or multicore parallelism.

The next concurrency phase must build on explicit frames, root suspended task
state, distinguish cancellation from failure, and define structured cleanup before
exposing spawn/await. A deterministic scheduler and equivalent C/Python/Rust/Pascal
observational tests are required before accepting these constructs.
libuv is optional future integration, never a default generated C11 dependency.
