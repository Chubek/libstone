# Prolific — compiler and interpreter profiling

Admission checks payload budgets before copying/scanning event text. Over-budget
inputs leave span state unchanged and count as collection loss. Serialization
checks its wire budget before every append and rejects findings attached to
failed or unrequested analyses.

Prolific is an instrumentation-based C++20 profiler with a versioned C11 ABI,
durable LMDB caching, JSON reports and specialized Lua analyzers. It observes
errors and optimization opportunities; it does **not** prove correctness or
apply optimizations. See [DESIGN.md](DESIGN.md) for invariants and scope.

## Build

From the LibStone root:

```sh
cmake -S moonstone -B build-moonstone -DMOONSTONE_BUILD_TESTS=ON
cmake --build build-moonstone --parallel
ctest --test-dir build-moonstone --output-on-failure
```

Prolific is enabled by `MOONSTONE_BUILD_PROLIFIC` (default ON). It requires the
existing `third_party/lmdbxx`, `third_party/lua`, `third_party/kaguya` and an
installed LMDB C library/header (e.g. `liblmdb-dev`). CMake diagnoses missing
dependencies; it never downloads them. Disable Prolific explicitly to build
other Moonstone modules without LMDB. Lua is compiled from the vendor sources,
not linked to a potentially incompatible system Lua. The bridge adapts the
checked-in Lua 5.5 state/GC APIs locally, without modifying vendors.
The embedded Lua/Kaguya symbols are hidden on ELF platforms to avoid binding to
an embedding compiler/interpreter's unrelated Lua ABI.

CMake targets: `Moonstone::Prolific` (recorder, cache, C ABI) and
`Moonstone::ProlificLua` (Lua replay). Link these from a Moonstone subdirectory
build. Public source-tree headers live under `prolific/`; installed headers
under `moonstone/prolific/`. This adds library/header installation, not a complete
Moonstone `find_package` export. Linux additionally builds
`libmoonstone_prolific_inject.so`.

## Embed in any language implementation

Instrument pass entry/exit, VM function calls, dispatch, counters, allocations,
frees and observed faults using `Recorder`/`Scope` or `prolific.h`. For example:

```cpp
#include "prolific/Prolific.hpp"
using namespace moonstone::prolific;
Recorder profiler;
{
    Scope pass(profiler, Domain::Compiler, "constant-fold", {"input.lang", "ir:17", 8, 3});
    profiler.emit(Domain::Compiler, Kind::Counter, "folded.nodes", 12);
}
Report report;
report.data = profiler.snapshot();
auto json = report.json();
```

Check admission results in real adapters; failed `Scope::result()` means that
span was not admitted. `Scope` destructor failures are counted as collection
loss. `record_loss()` accounts for events an adapter loses before admission.
The C ABI exposes `prolific_create/begin/end/emit/report/save/destroy`; errors
are status codes with `prolific_last_error()` on the calling thread. The ABI
copies strings and never retains host pointers. Locations use UTF-8 text,
1-based host-supplied line/column positions (zero means unavailable), and
unmodified string IR IDs. `examples/host.cpp` and `tests/c_api.c` are runnable
embedding examples.

Spans are nested, thread-affine and LIFO. Calls and snapshots serialize and can
block an OS thread; callbacks never execute on producer threads. Destroy only
after producers are joined. Async spans, task migration, signal handlers and
fork-without-exec are unsupported. Timings are steady-clock elapsed times
(including scheduling/blocking and instrumentation overhead), not CPU times.
Exclusive time subtracts completed immediate child spans across all domains.

Defaults: 100,000 events, 64 MiB event storage accounting, 4,096 bytes per text
field, 256 lifetime thread identities, depth 256. These bound retained payload
and admission, not the process RSS or transient report/snapshot copies. Limits
can be configured in C++ or C. Exhaustion, invalid recording and adapter loss
are tracked explicitly. The v1 hard limits are a million events and 1 MiB per
text field; invalid limit configurations are rejected. Recording failures
increment `rejected`; unclosed spans remain in `open_spans`. `complete` is true
only when both are zero, and only describes **supplied instrumentation**, not
whether the host instrumented every relevant operation. Unknown host operations
should be emitted as named observations, not represented as safe operations.

## Inject into a native runtime (Linux, GCC/Clang)

Rebuild the host with `-finstrument-functions`, then:

```sh
PROLIFIC_CACHE="$PWD/profile-cache" PROLIFIC_RUN=compile-1 \
LD_PRELOAD="$PWD/build-moonstone/prolific/libmoonstone_prolific_inject.so" ./compiler input.lang
build-moonstone/prolific/prolific report profile-cache compile-1 > report.json
```

This captures native function spans via `__cyg_profile_func_enter/exit`.
`PROLIFIC_CACHE` enables collection; without it the injector is inactive.
`PROLIFIC_RUN` defaults to a process/time-based ID. The fixed injection stack
holds 512 frames; the recorder's stricter default depth still applies. The
injector serializes callbacks against shutdown, suppresses self-recursion, and
saves at orderly process exit. It reports persistence failures to stderr without
changing the host's exit status. Join host workers before exit for closed spans.
Crashes, `_exit`, `SIGKILL`, `longjmp` and unloading the library while workers
are active do not provide complete finalization. Use explicit C ABI snapshots
for checkpoints. Duplicate run IDs fail instead of overwriting data.

No binary rewriting or arbitrary attachment is implemented: an uninstrumented
binary cannot provide these events. Native addresses are retained as address
identities; ASLR makes them run-local. Symbolization, native source attribution,
sampling, automatic malloc interception and platform-specific debugger adapters
are not implemented. Language-specific hooks supply richer compiler/VM events.

## Specialized Lua profilers

The Kaguya-backed libraries are embedded modules supplied to each replay VM,
not standalone modules for an unrelated system Lua executable:

```lua
local cc = require("lprofcc")
local vm = require("lprofvm")
local dispatches = 0

cc.on("end", function(e)
    if tonumber(e.duration_ns) > 10000000 then
        cc.finding("note", "HOT-PASS", "Investigate " .. e.name)
    end
end)
vm.on("counter", function(e)
    if e.name == "opcode.add" then dispatches = dispatches + e.value end
end)
vm.on("error", function(e)
    vm.finding("error", "VM-FAULT", e.detail)
end)
vm.finish(function(summary)
    vm.finding("note", "ADD-DISPATCHES", tostring(dispatches))
end)
```

- `on(kind, callback)` filters the module's domain. Kinds: `begin`, `end`,
  `instant`, `counter`, `error`, `allocation`, `free`, or `*`.
- `finish(callback)` runs after all events. Its summary contains `complete`,
  `rejected`, `open_spans`, `event_count`. Inspect completeness before drawing
  conclusions from counts. Finish findings reference the whole run (sequence 0).
- `finding(severity, code, message)` records a heuristic finding associated with
  the current event, or the whole run during the same module's finish callback.
  Severity is `note`, `warning` or `error`. It cannot be called during setup.
- Registration happens only during script setup, with at most 256 callbacks
  across both modules. Events replay in sequence order; matching callbacks and
  finish callbacks run in registration order. Runtime-domain native spans remain
  in the base report; these two libraries filter compiler and VM events only.
- Event fields mirror JSON. Lua sequence, timestamp, thread, span, parent,
  duration and self time are **decimal strings** for exact 64-bit transport.
  Signed `value` is a Lua integer; source locations and IR IDs are preserved.
  Callbacks receive fresh tables; mutation cannot change the underlying events.
- Defaults: 1 MiB source, approximately one million Lua VM instructions (100-op
  hook granularity), 1,000 findings, 4,096 bytes per finding field. Script errors,
  uncaught native API errors and detected exhaustion produce `analysis.status`
  `failed`, preserve raw events, and discard partial findings. Successful replay
  is `completed`, even if collection itself was incomplete.
  Finding/registration exhaustion remains a failure even if a script catches the
  corresponding Lua error; it cannot silently truncate a successful analysis.

```sh
build-moonstone/prolific/prolific-host profile-cache
build-moonstone/prolific/prolific list profile-cache
build-moonstone/prolific/prolific analyze profile-cache example \
    moonstone/prolific/examples/specialized.lua analyzed-example > analyzed.json
```

Omit the final output run ID to analyze read-only without caching new results.
The CLI returns 0 on success, 1 for usage/cache/I/O errors, 2 for failed Lua
analysis (with the failed report on stdout).

Only base/table/string/math/utf8 libraries and a two-module `require` are exposed;
filesystem loading, OS, I/O, debug and arbitrary module loading are absent.
**This is not a sandbox.** Scripts share the native process and memory; native
calls are not instruction-budgeted, Lua errors can be caught, and Lua can still
allocate unbounded memory. Run only trusted scripts, or isolate the entire
profiler with OS resource limits in another process. No safety result or repair
is authorized by successful profiling.

## Cache and report format

`Cache` uses actual vendored lmdbxx over the LMDB C library, with ordinary
synchronous commits (no `NOSYNC` or `WRITEMAP`). One live cache object per
canonical path per process is enforced; methods serialize on that object and
LMDB synchronizes independent processes. Read-only opens never create paths.
Run IDs are 1–128 ASCII letters/digits/`.`/`_`/`-`. Each write atomically commits
`snapshot/RUN` and `report/RUN`. Replacement requires explicit `replace=true`.
Full maps and storage errors abort without modifying earlier runs. Map size
defaults to 256 MiB and is configurable in C++; there is no automatic resizing.
Copy or inspect LMDB environments using LMDB's own backup tooling, not raw copies
of actively written mappings.

The binary v1 snapshot is little-endian and length-delimited, with magic/version
checking, UTF-8 validation, bounded counts/strings, exact signed integer transport,
LIFO/thread/parent validation and duration consistency checks. Values are bounded
to 128 MiB; decoding allows at most a million events, 100,000 findings and 1 MiB
per string. There is no cryptographic authentication or cross-version migration.
Do not concurrently alter the cache outside LMDB transactions.

JSON schema `moonstone.prolific/1` has these stable top-level fields:

| Field | Meaning |
| --- | --- |
| `created_unix_ns` | Recorder creation time, UTC epoch nanoseconds |
| `complete`, `rejected`, `open_spans` | Collection quality (independent of analysis success) |
| `analysis` | `status`: `not_requested`, `completed`, `failed`; `error`: cause |
| `events` | Ordered raw events with relative ns timestamps and host locations |
| `timings` | Per-domain/name completed-span count, inclusive/exclusive/min/max ns |
| `samples` | Per-domain/kind/name counter/alloc/free count, sum, min/max |
| `findings` | Script severity/code/message, event sequence, `evidence: heuristic` |

Sample values are host-defined measurements. Their sum is not automatically a
live heap size, opcode frequency or rate; distinguish deltas from gauges in your
adapter. A sample sum overflowing signed 64 bits becomes `null` with
`overflow: true`; timing totals saturate with an overflow flag. No silent wrap.
JSON carries exact decimal integer tokens; consumers using floating-point JSON
numbers must account for precision beyond 2^53. Text escaping and locale-independent
numeric output produce valid UTF-8 JSON.

## Verification and limitations

`tests/` covers nested/thread-affine spans, original identities, signed extremes,
UTF-8/control escaping, admission loss, concurrency, corrupt/truncated snapshots,
actual LMDB persistence/reopen/rollback on map exhaustion, actual Lua callbacks,
analysis failure/exhaustion/finalization, plain-C ABI and real Linux preloading.
The optional Python-standard-library CLI test parses emitted JSON and checks
offline replay. Public headers compile independently with warnings as errors.
See [STATUS.yaml](STATUS.yaml) for implemented and unsupported features. This is
a working baseline, not a production-grade profiler or semantic error detector.
Compiler, sanitizer, installation and root-smoke verification scope is recorded
in [VALIDATION.md](VALIDATION.md).
