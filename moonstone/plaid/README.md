# Plaid

Plaid is Moonstone's partial, tested C++20 concurrency/parallelism toolkit.
It is divided into `continuum`, `weaver`, `loom`, `conduit`, `chorus`,
`parallax`, `sentinel`, `ir`, `runtime`, and `integration`.

## Reference functionality

- **Continuum:** once-only callback continuation; explicit pure value CPS graph
  with yield/resume; synchronous named effect handler dispatch.
- **Weaver:** conservative CFG backward liveness identifying frame spills at
  modeled suspension points.
- **Loom:** explicit manually driven C++20 coroutine; cooperative scheduler;
  mutex-guarded work-stealing thread pool with futures, optional queue limits,
  serialized concurrent shutdown, worker-safe stop requests and task scopes.
- **Conduit:** bounded and unbuffered channels, blocking/try APIs, close;
  per-offer rendezvous acknowledgment; cancellable receive-only selection across
  buffered channels with scoped observer cleanup.
- **Chorus:** serial local actors on a worker pool, FIFO mailbox and failure
  reporting, optional mailbox limits and nonjoining close.
- **Parallax:** explicit chunked parallel-for/reduce, portable scalar lane
  mapping, bounded affine memory dependence check with `Unknown` status.
- **Sentinel:** vector clocks, offline trace races and lock-order cycle
  detection. No automatic instrumentation.
- **IR:** typed-ish opcode vocabulary and use-before-definition verifier;
  not a compiler's complete typed concurrency IR.
- **Runtime:** cooperative C ABI for embedding in non-C++ compilers.

All public C++ code is header-only except `runtime/PlaidRuntime.cpp`; compile
with C++20 and link platform thread support. The C ABI builds as a static
library. Build/test at the enclosing `moonstone/CMakeLists.txt` as described
in `../README.md`.

## Read this first

`AGENTS.md` specifies missing semantics, correctness requirements, production
acceptance and how to integrate existing LibStone Firestone/Wetstone/Chisel
facilities. `STATUS.yaml` distinguishes implementation from missing features.
`manual/01-continuations.md` through `manual/06-runtime-hardening.md` describe the
current APIs and their limitations. Running the unit tests does **not** prove
that Plaid is safe for untrusted workloads or production services.

The P0 hardening suite covers concurrent stop/admission, MPMC channel close,
select cancellation/cleanup, actor lifetime and reduction admission failures.
Channel/actor messages must be noexcept-movable and noexcept-destructible;
`stop()` now rejects worker self-join with an exception. See
`manual/02-runtime.md` for the C++ 0.2 compatibility tightening.
