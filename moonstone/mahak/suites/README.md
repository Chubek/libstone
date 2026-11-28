# Cephyr benchmark suites for Mahak

Three manifests measure Cephyr as it exists today. They take explicit tool
paths, so the same file measures a baseline build and a candidate build without
editing the suite:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mahak -j

mahak validate moonstone/mahak/suites/cephyr-optimization.json
mahak run moonstone/mahak/suites/cephyr-optimization.json \
  --store /tmp/cephyr-profile \
  --param compiler="$PWD/build/cephyr" \
  --param target=x86_64-linux-gnu
mahak compare <baseline-session> <candidate-session> --store /tmp/cephyr-profile
```

`compiler` is the Cephyr executable. It defaults to `cephyr` on `PATH` but is
normally overridden. `target` defaults to `x86_64-linux-gnu`. An override may
name a value that is not in the declared matrix: overriding a tool path is the
intended use.

## `cephyr-optimization.json`

Blackstone off (`--no-blackstone`) versus on (`--blackstone`), with FactIR
disabled on both sides so the MIR rewrite pass is the only variable.

| Benchmark | Purpose |
| --- | --- |
| `phi-join`, `cse-dominance` | cross-block facts and dominance-based CSE |
| `stress-dense`, `stress-sparse` | rewrite pressure |
| `stress-inert` | what every user pays when the pass finds nothing |
| `join-disagree-control` | two branches supply *different* constants; an unsound fold fails the exit-code oracle |
| `memory-effects-control` | load/store side effects must survive |

Each case records `compile_wall` (build metric), `wall`, `cpu_user`,
`cpu_system`, and `max_rss` (runtime metrics). Warm-up is excluded, raw samples
are retained, and an oracle failure invalidates the timing samples instead of
dropping them.

## `cephyr-incremental.json`

`full-build` compiles five translation units and links them; `single-unit`
compiles one unit (compile-only, no oracle). Compare the two sessions to price a
one-unit rebuild against a full rebuild, which is the number an incremental
cache must beat.

`fixtures/project/shared.h` defines `EXPECTED_SUM`. Editing it changes the
program's expected answer, so a stale cached object fails the oracle rather than
producing a fast, wrong build. The private header `private_a.h` is included by
exactly one unit.

Not observable through a manifest today, and therefore not reported: the number
of translation units actually recompiled, and `.text` size. Both need a runner
script or an SDK measurement; Mahak never invents them.

## `cephyr-abi.json`

By-value aggregate interoperability in both directions, plus an all-reference
control:

- `cephyr-caller`: Cephyr compiles the caller, the reference compiler the callee.
- `cephyr-callee`: the reverse.
- `reference-control`: both from the reference compiler.

The fixture pair covers a register-passed record, a mixed integer/floating
record, and a five-word stack-passed record. The control proves the pair is
sound before any cross-compiler result is read.

On this host both Cephyr directions are a **coverage gap**, recorded as
`build_failed` with Cephyr's own diagnostic:

```
fixtures/abi_callee.c:6: error[cephyr.abi.aggregate]:
  by-value aggregate ABI is only supported on the bundled RV64IM LP64 backend
```

That is the intended outcome: an unsupported feature is a labelled gap, never a
pass. Compiling the same pair with `--target=riscv64-linux-gnu` also rejects the
`double` member, which matches the documented integer/pointer restriction; a
compile-only RV64 case is the natural next addition.

## Reading the first measurements

One release run of `phi-join` on this host:

- `compile_wall`: 35.6 ms (`--no-blackstone`) vs 28.8 ms (`--blackstone`).
- `wall`: median ~220 µs with one ~2.2 ms outlier per side.

The kernel runs for about 200 µs, so process start-up dominates `wall`; at this
size a runtime difference is not interpretable. `compile_wall` is the only
metric with signal in this suite until a long-running, runtime-input kernel with
a checked checksum is added. Those numbers are a single observation, not a
conclusion about the pass.
