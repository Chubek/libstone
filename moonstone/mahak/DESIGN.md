# Mahak — architecture and interface contract

Mahak is the Moonstone benchmarking subsystem for compilers, interpreters,
virtual machines, runtimes, libraries and native executables. It is a C++20
library (`moonstone::mahak::v1`), an installable CMake target (`Moonstone::Mahak`),
a command-line tool (`mahak`), and a small stable C ABI (`mahak.h`).

This document is the authoritative contract between Mahak modules. Public
headers in `include/mahak/` define the types; `src/` implements them. Changes to
a published header require an explicit contract revision and corresponding
tests.

## 1. Layers and ownership

| Layer | Public header(s) | Implementation files | Worker territory |
| --- | --- | --- | --- |
| JSON, RNG, shared model helpers | `Json.hpp`, `Rng.hpp`, `Model.hpp` | `src/Json.cpp`, `src/Rng.cpp`, `src/Model.cpp` | Orchestrator foundation |
| Manifest + planning + adapters | `Manifest.hpp`, `Adapter.hpp` | `src/Manifest.cpp`, `src/Adapter.cpp` | Worker 3 |
| Process supervision + scheduling | `Execution.hpp` | `src/Process.cpp`, `src/Execution.cpp`, `src/Scheduler.cpp` | Worker 4 |
| Measurement + environment + perf | `Measurement.hpp` | `src/Measurement.cpp`, `src/Perf.cpp`, `src/Environment.cpp` | Worker 5 |
| Statistics | `Statistics.hpp` | `src/Statistics.cpp` | Worker 6 |
| Storage + reporting + CLI + C ABI | `Storage.hpp`, `Report.hpp`, `Cli.hpp`, `mahak.h` | `src/Storage.cpp`, `src/Report.cpp`, `src/Cli.cpp`, `src/CAPI.cpp` | Worker 7 |
| Tests, fixtures, docs, verification | — | `tests/`, `manual/`, `suites/` | Worker 8 |
| Integration + engine | `Engine.hpp` | `src/Engine.cpp` | Worker 1 |

`coordination/` is development infrastructure and is **not** linked into
`Moonstone::Mahak`; its code never runs on a measured path.

## 2. Error model and exit codes

Mahak reuses `moonstone::Error` / `moonstone::Result<T>` from
`moonstone/core/Result.hpp`. An implementation must not return a fabricated
success value when an operation is unsupported or partially completed.

- `ErrorCategory::InvalidInput` — malformed manifest, parameters, options.
- `Unsupported` — capability absent (e.g. perf on this platform, persistent
  mode not implemented by the target).
- `BudgetExhausted` — explicit resource limit reached.
- `NotFound` — missing session/artifact/tool.
- `Internal` — harness invariant failure.

CLI exit codes: `0` success, `1` internal, `2` invalid configuration,
`3` execution failure, `4` confirmed regression, `5` not found.

## 3. Manifest v1

Manifests are strict JSON. Unknown keys are errors. A manifest is loaded with
`load_manifest(path)`, which resolves `base_directory` to the manifest's parent
directory. Paths inside the manifest are relative to that base directory unless
absolute. Only JSON is supported; there is no YAML/TOML dependency.

```json
{
  "schema": 1,
  "id": "mahak-core",
  "version": "1.0.0",
  "description": "Core compiler and runtime benchmarks",
  "seed": 20260101,
  "defaults": {
    "warmup": 1,
    "repetitions": 5,
    "timeout_ms": 30000,
    "measurement": "warm-process",
    "adaptive": false,
    "target_relative_stddev": 0.02
  },
  "targets": [
    {
      "id": "cc-o2",
      "kind": "compiler",
      "description": "System C compiler at -O2",
      "stages": [
        {
          "kind": "compile",
          "commands": [
            {
              "argv": ["cc", "-O2", "-o", "{artifact:prog}", "{fixture:source}"],
              "artifacts": ["{artifact:prog}"]
            }
          ]
        }
      ],
      "execute": {"argv": ["{artifact:prog}", "{param:size}"]}
    }
  ],
  "benchmarks": [
    {
      "id": "kernel",
      "description": "Computational kernel",
      "group": "kernels",
      "tags": ["cpu", "numeric"],
      "modes": ["end-to-end", "throughput"],
      "fixtures": [{"name": "source", "path": "fixtures/kernel.c"}],
      "parameters": [{"name": "size", "values": [64, 128, 256]}],
      "oracle": {"kind": "regex", "pattern": "checksum=[0-9a-f]+"},
      "metrics": [
        {"name": "wall", "unit": "ns", "direction": "lower", "primary": true},
        {"name": "compile_wall", "unit": "ns", "direction": "lower"}
      ],
      "sampling": {"repetitions": 5}
    }
  ]
}
```

### 3.1 Placeholders

Placeholders are expanded during planning, before any process runs:

- `{benchmark}`, `{target}`, `{case}`
- `{param:NAME}` — current matrix value rendered as a string
- `{fixture:NAME}` — copied fixture path inside the case work directory
- `{artifact:NAME}` — artifact path inside the case work directory
- `{source}` — first fixture
- `{output}` — first artifact
- `{work}` — case work directory

An unresolved placeholder is a planning error. Leaves with the same name across
target/benchmark are a validation error.

### 3.2 Matrix and exclusions

Benchmark parameters and target parameters are merged; the planner computes the
Cartesian product in declaration order (target parameters first, then benchmark
parameters). `parameter_overrides` in `PlanOptions` pins a parameter to a single
value. `exclusions` suppress a (benchmark, target) pair, optionally with a
reason; excluded cases are omitted and the omission is recorded in the session
notes. `PlanOptions` filters (`benchmarks`, `targets`, `groups`, `tags`,
`modes`) select cases; an empty filter means "all". Tag filters require at
least one tag match.

### 3.3 Stages and execution modes

Stage order is fixed: Setup, Compile, Link, Validate, Execute, Teardown.
Target stages are merged with benchmark stages. If both provide an Execute
command, the benchmark's command wins. `modes` restrict which stages run:

- `compile-only`: through Compile.
- `link-only`: through Link.
- `execute-only`: Execute only (no build stages).
- `end-to-end`, `startup`, `throughput`, `scalability`: all stages.

If the selected modes require a stage the plan does not contain, planning fails
for that case with an explicit diagnostic.

Artifacts are paths inside the case work directory. Fixtures are copied into
`<work>/fixtures/<name>` before Setup; the source manifest path is never
mutated. Work directories are private (mode 0700) and removed unless
`keep_work_directories` is set.

## 4. Execution contract

- No shell is ever invoked. `argv` is passed directly to `execvp`/equivalent.
- Child processes get their own process group. Timeout/cancel sends SIGTERM,
  waits a short grace period, then SIGKILL; descendants are reaped.
- stdout/stderr capture is bounded per stream (`max_output_bytes`). Excess is
  drained and discarded, and `output_truncated` is set. Capture never blocks
  the child forever.
- `CommandResult.validity` is one of the `Validity` values and must reflect the
  actual outcome. `exit_code == -1 && signal == 0 && spawn_failed` is an
  explicit harness failure, not a successful zero.
- `rusage` values come from `wait4`/`RUSAGE_CHILDREN`; unavailable fields are
  noted in `usage.usage_notes`, never reported as plausible zero.
- A measurement is only valid if the Execute stage completed and the oracle
  passed. Compile/link time metrics are recorded for build stages, but they are
  labelled by metric name and never conflated with runtime measurements.
- Sampling modes are recorded in `SampleRecord`/`RunRecord`: cold process
  (fresh process per repetition), warm process (same binary, fresh process per
  repetition after warm-up), and persistent process (line protocol). Results
  from different modes are never merged.
- Warm-up samples are not included in measured observations.
- Adaptive sampling stops early only after the configured minimum repetitions,
  and never suppresses a large/late-timeout outlier. Raw observations are
  always retained.
- Uncontended execution is the default. `allow_concurrent` is recorded as a
  measurement condition and requires explicit opt-in.
- CPU affinity and resource limits are applied when supported and recorded.

### 4.1 Persistent process protocol

`run_persistent` starts the target once. Each iteration writes one line to
stdin (`iteration_input` + `\n`) and reads one newline-terminated line from
stdout; the time between the write and the completed line is the observation.
A process that exits, times out or emits no line yields an invalid observation
with an explicit validity code. This is a documented protocol, not a claim of
universal persistent-process support.

## 5. Measurement contract

- Wall clock is `CLOCK_MONOTONIC` nanoseconds.
- CPU time is user+system nanoseconds from `wait4`; peak RSS is in bytes.
- `perf_event_open` counters are optional. Missing permission, unsupported
  events, multiplexing and scaling are recorded in `PerfCounter.note`,
  `enabled` and `scale`. An event that could not be measured is
  `enabled=false`; it is never `value=0` without that marker.
- Compiler/runtime instrumentation uses the documented metric protocol:

```text
MAHAK-METRIC {"name":"gc_pause_ns","unit":"ns","value":1234,"source":"runtime"}
MAHAK-PHASE  {"name":"optimize","category":"compiler","start_ns":10,"duration_ns":20}
```

Malformed records produce warnings and are ignored. `Moonstone Prolific`
events are accepted through an adapter that maps Prolific's recorder snapshot
into `PhaseEvent`/`RuntimeMetric`; if Prolific is not enabled, Mahak reports
that the integration is unavailable rather than emitting zeros.
- Environment metadata is probed from the OS; unavailable fields are empty.

## 6. Statistics contract

- Raw observations are preserved with units, provenance, validity and sample
  index. Invalid observations are excluded from estimators but remain in the
  session and exports.
- Estimators: min, max, mean, median, sample variance (n-1), standard
  deviation, MAD, q1/q3, p90/p95/p99, relative standard deviation.
- Quantiles use Hyndman-Fan type 7 over sorted valid values.
- Confidence intervals are percentile bootstrap with a deterministic seeded
  RNG; fewer than two valid samples is `defined=false`.
- Outlier detection is modified Z-score over MAD; excluded values are retained
  and labelled.
- Comparisons respect directionality. For lower-is-better, a candidate ratio
  below 1 is an improvement; for higher-is-better, above 1 is an improvement.
  A regression requires the configured practical threshold *and* statistical
  evidence when enabled. Insufficient data is a verdict, not a regression.
- Paired comparisons pair by observation sequence; unpaired comparisons use an
  independent-sample bootstrap. Incompatible units/metric identity is an error.
- Scalability uses a log-log fit `y = a*x^b`; fewer than two positive points is
  `defined=false`.

## 7. Result storage contract

`SessionRecord` is the canonical result document. JSON is lossless; schema
version is explicit. Unknown future schema versions are rejected. v0 (flat
legacy run lists) migrates to v1 through `migrate_session`. CSV is one row per
raw sample with a stable header.

`HistoryStore` commits atomically: write temp, fsync, rename, fsync directory,
then update the index. A store lock serializes writers. `recover()` removes
incomplete temp files and rebuilds a missing/corrupt index. A session id may be
committed once; an identical re-commit is idempotent, a different document is
rejected.

## 8. Reporting contract

- Terminal/Markdown/HTML/CSV reports always include raw units, sample counts,
  uncertainty (when defined), missing metrics, validity markers, and
  environment differences.
- Compiler phase breakdowns and optimization-level comparisons are grouped by
  configuration, never merged.
- Historical trends order sessions by creation time and label incomplete data.
- HTML is standalone (inline CSS, no external assets/scripts).
- A regression summary is only produced from `ComparisonResult.regression`.

## 9. Embedding and build contract

- `MOONSTONE_BUILD_MAHAK` (default ON) builds `moonstone_mahak` and exports
  `Moonstone::Mahak`. Existing Moonstone options are respected:
  `MOONSTONE_BUILD_TESTS`, `MOONSTONE_BUILD_TOOLS`, standalone builds, and
  installation/export.
- `MOONSTONE_BUILD_MAHAK_COORDINATION` (default OFF) builds the separate
  development coordination tools. It is never linked into `Moonstone::Mahak`.
- Public headers compile in isolation with `-Wall -Wextra -Wpedantic -Werror`.
- The C ABI is strict C11 and tested by a C translation unit.
- `cmake --install` installs headers, the `mahak` tool, and the export set; a
  relocated package consumer builds and runs.

## 10. Non-goals and explicit limitations

- Mahak does not sandbox untrusted benchmark targets. It uses process
  isolation, timeouts and optional resource limits; that is not a security
  boundary.
- Mahak does not provide a universal persistent-process protocol, distributed
  execution, or statistical proof of causality. It reports measurement
  conditions and uncertainty.
- `perf` availability is platform- and permission-dependent. Missing counters
  are reported as unavailable.
- The development coordination layer is not part of the measured execution path
  and does not use POSIX message queues during a benchmark run.

## 11. Additive contract revision (v1.1)

Four headers extend the v1 surface without changing an existing declaration:

| Header | Surface | Guarantees it encodes |
| --- | --- | --- |
| `Plugin.hpp` | native plugin ABI (`mahak_plugin_init_v1`), size-tagged host/plugin tables, typed registration, `PluginRegistry` | reject ABI mismatch, undersized tables, duplicate ids and missing mandatory hooks; teardown before unload; no fabricated capability |
| `Runner.hpp` | Lua runner/extension hooks, `RunnerLimits`, `HookComposition`, `RunnerFingerprint` | JSON remains mandatory; a script-only run is a configuration error; ambiguous judging must name a composition; hook identity is persisted with the session |
| `Profile.hpp` | named profiles, `ExecutionFingerprint`, `ReplayPolicy`, `DotOptions` | stable id differs from display name; a replay adds no timing samples; canonical JSON stays authoritative and the index is rebuildable |
| `Recommend.hpp` | oracle facts, recommendation status/source, `Recommender` | legality facts and performance observations stay separate; `BudgetExhausted` is never success; a model suggestion that fails the same checks is downgraded, not promoted |

`Version.hpp` constants stay unchanged; each header pins its own version
(`mahak_plugin_abi_version`, `mahak_runner_api_version`,
`mahak_profile_schema_version`, `mahak_recommend_api_version`). The umbrella
header includes all four, and `tests/contract.cpp` pins the versions, table
shapes, id rules and fingerprint determinism.

Not yet wired: selecting a plugin oracle or a runner from a manifest requires an
additive field on `EngineOptions`/`OracleSpec`, which lands with the SDK
implementation together with the `libmahaksdk` shared library. Until then a
plugin can be loaded and its hooks called through `PluginRegistry`, but a
manifest cannot name it. This is stated rather than implied so no specification
appears to support more than the build does.
