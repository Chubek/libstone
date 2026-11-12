# Production roadmap

Refer to `plaid/AGENTS.md` for complete acceptance criteria. The most
valuable initial next steps are (1) stress the thread pool/channel/actor
lifecycle with sanitizers and randomized schedules, (2) implement genuine
`async_send`/`async_recv` without blocking worker threads, (3) define an
atomic cross-channel select registration protocol including unbuffered
channels, (4) build an actual Firestone CPS adapter and typed control-effect
lowering, (5) integrate continuation spill frames with Chisel and moving GC,
(6) implement source-aware diagnostics and real static/dynamic race checking,
(7) add dependence legality and real SIMD vectorization, and (8) expose
portable backend/runtime APIs to compiler frontends.

The P0 pass in `06-runtime-hardening.md` covers selected lifecycle failures,
bounded admission and sanitizer stress, not all of step (1). Remaining work
includes timed/cancellable rendezvous, deterministic wakeup injection, nested
scope scheduling, exhaustive failure injection and a thread-safe/reentrant C
runtime. Do not treat the recorded sanitizer passes as completion of P0–P5.

Do not replace concrete implementations with files that only declare classes.
Each PR needs executable tests demonstrating a positive use case, an invalid
use case, cancellation or error propagation, and a relevant race/failure
scenario. End-to-end compiler integration must prove that generated programs
run correctly, not merely that IR validators accept them.
