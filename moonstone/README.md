# Moonstone — program safety and language runtime toolkits

Moonstone is a collection of reusable C++20 components for language implementations,
intended to be overlaid at `libstone/moonstone/`. It now comprises **five** subsystems:

| Library | Functionality present in this snapshot | Not yet present |
| --- | --- | --- |
| Chisel | Small CFG ownership analysis, explicit loans/regions, demonstration rooted GC | Rust-like NLL/Polonius, proven memory safety, production MemTKX integration |
| Syren | S-expression diagnostic rules, stderr/SARIF, repair callback, runtime recorder | Complete error catalog, independent repair verification pipeline |
| Supersonic | Bounded integer-expression synthesis, CEGIS callback, Lua SynthGram module | Fully typed general synthesis, native Satie/e-graph solver integration |
| ExoML | Restricted DSL interpreter and declaration-model validation | Complete ExoML grammar, hatching, real header parsing, binding generation |
| **Plaid** | One-shot continuations, explicit pure CPS machine, synchronous effect handlers, coroutine-frame liveness, C++20 manual coroutine, cooperative scheduling, work-stealing pool, structured task scope, bounded/rendezvous channels, buffered-channel select, serialized local actors, parallel loop/reduce, lane mapping, bounded affine-dependence checking, trace-based races and lock order, C runtime ABI | Arbitrary Scheme call/cc, delimited stack capture, multi-shot resource-safe continuations, compiler async lowering/codegen, stackful fibers, async I/O, full atomic Go select, native SIMD vectorizer, GPU lowering, instrumenting race detector |

**Status:** working, tested implementation baseline. It must not be represented as a
production-ready concurrency runtime, borrow checker, vectorizer or compiler pipeline.
`AGENTS.md` and `plaid/AGENTS.md` provide detailed implementation steps, invariants,
and acceptance gates. All APIs exposing a partial feature are documented as such.

## Building

From the LibStone repository root (after extracting this archive):

```sh
cmake -S moonstone -B build-moonstone -DCMAKE_BUILD_TYPE=Debug \
  -DMOONSTONE_BUILD_TESTS=ON -DMOONSTONE_BUILD_PLAID=ON
cmake --build build-moonstone --parallel
ctest --test-dir build-moonstone --output-on-failure
build-moonstone/plaid-continuations
build-moonstone/plaid-csp-actor
```

The P0-hardened tree has **20 CTest tests**, including fourteen Plaid tests
and the original six Moonstone tests. Public Plaid headers compile in isolation
with warnings as errors, and the C ABI consumer uses strict C11. For current
sanitizer and stress validation see `plaid/manual/06-runtime-hardening.md`;
`docs/PROVENANCE.md` describes the original snapshot's verification scope.
The package is also designed to build standalone with CMake >=3.20, C++20 and
OS threads. CI across architectures, sanitizer runs and integration with the
complete LibStone build are separate acceptance gates.

## Using from C++

```cpp
#include "moonstone/plaid/Plaid.hpp"
using namespace moonstone::plaid;
loom::WorkStealingPool workers(4);
conduit::Channel<int> messages(8);
workers.submit([&]{ messages.send(42); }).get();
int answer = *messages.receive();
```

`WorkStealingPool`, `Channel`, `Actor` and `TaskScope` have distinct safety rules:
read `plaid/manual/02-runtime.md` before using them in a runtime. A channel
operation that blocks must not be executed on a single-thread event loop.

A pure CPS example is in `plaid/tests/cps_machine.cpp`; a C ABI example is
in `plaid/tests/c_api.c`. Standalone Plaid C++ API includes `plaid/Plaid.hpp`.

## Configuring dependencies

The repository's vendored `third_party/` directory is **not** shipped in this
archive. CMake's `MOONSTONE_VENDOR_ROOT` defaults to `../third_party`. Optional
MemTKX, Satie and Equinox-NG switches only make upstream headers available;
they do **not** activate complete integrations. Inspect actual upstream APIs
before implementing adapters. Existing Firestone `libcps`, `libconcurrency`,
`libcompute`, `libintrin`, and Wetstone analysis should be reused, not duplicated.

The current GitHub repository did not contain a committed `moonstone/` folder
at inspection time, so this archive retains the complete previously generated
Moonstone baseline while adding Plaid. No changes to the root LibStone checkout
have been pushed or applied. See `cmake/ROOT-INTEGRATION.cmake.txt` for opt-in
root-CMake registration.

## Source tree

- `core/`: common structures shared by Moonstone.
- `chisel/`, `syren/`, `supersonic/`, `exoml/`: preexisting components and tests.
- `plaid/continuum/`: one-shot continuations, prompts, effect dispatch, reference CPS machine.
- `plaid/weaver/`: frame liveness across suspension points.
- `plaid/loom/`: cooperative and multithreaded scheduling, cancellation, task scopes, manual coroutines.
- `plaid/conduit/`: channels, buffered selection and wake notifications.
- `plaid/chorus/`: single-handler local actor mailboxes.
- `plaid/parallax/`: explicit parallel algorithms, conservative dependence checks.
- `plaid/sentinel/`: vector-clock traces and lock-order checks.
- `plaid/ir/`: small validating Plaid instruction vocabulary.
- `plaid/runtime/`: stable versioned subset of a C ABI.
- `plaid/integration/`: normalized Moonstone diagnostic and suspension-safety contracts.

## Safety and scope

- `plaid::continuum::OneShot` guards exactly-once callback invocation; it does not
  capture or restore arbitrary native stacks. The pure CPS machine can clone
  **only pure value snapshots**; this must not be generalized to resource-owning frames.
- `EffectScope` synchronously routes handlers; it is not a resumable algebraic
  effect interpreter.
- `ManualCoroutine` is an explicitly resumed C++ coroutine using the compiler's
  state machine; it is not a production async/await scheduler or compiler pass.
- `weaver::analyze_frame` computes variables live after modeled suspend points;
  it does not generate executable native coroutine code.
- `conduit::select_receive` supports **buffered channels only**, a single
  receiving select operation, and timeout. Rendezvous select is rejected rather
  than incorrectly emulating atomic all-case selection.
- `parallax::lane_map` executes scalar lane operations; it is not an ISA
  vectorizer. `loop_dependence` only reasons about a bounded set of explicitly
  identified affine byte accesses; unknown effects must not be parallelized.
- `sentinel::races` analyzes supplied clocks and accesses; it does not instrument
  programs or infer synchronization edges.
- The C runtime is single-thread driven; lifetime and callbacks are owned by
  the caller. Do not destroy it from inside a callback or concurrently with run.

For production deployment, complete the agent acceptance plan, fuzz and stress
concurrent operations, and run ASan/UBSan/TSan on each supported platform.
