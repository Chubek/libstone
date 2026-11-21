# Cephyr rewrite benchmark

Build a Release compiler with `-DCEPHYR_ENABLE_MOONSTONE=ON`, then run the
benchmark on a supported Linux host:

```sh
python3 cornerstone/Cephyr/benchmark/run.py \
  --cephyr build-cephyr/cephyr --output /path/to/new/cephyr-results \
  --repeat 7 --assert-rewrites
```

The baseline runs without QML. The candidate selects the built-in
`canonicalize` pass. To measure a named profile or one or more custom options,
provide a config with no active default profile and pass arguments only to the
candidate:

```sh
python3 cornerstone/Cephyr/benchmark/run.py \
  --cephyr build-cephyr/cephyr --output /path/to/new/profile-results \
  --config path/to/Cephyr.toml --candidate-arg=--profile=my_profile
```

The corpus has six handwritten cases (including joins, loops, narrow integer
promotions, and memory and calls) and sixteen deterministic generated cases.
Generated checks use an independent Python integer model of unsigned LP64.
The four generated fixture families cover dense, sparse, and absent matches
for canonicalization, plus multiplication by two, at four input sizes. Use
`--fixture 'dense|inert'` for a subset.

For each workload the runner alternates invocation order, discards the warmup
samples, and measures `--emit-mir --no-cache` with Prolific. `summary.json`
contains timings, MIR bytes and instruction/opcode counts; the per-sample
Prolific reports and MIR are retained. `QML ms` is total invocation time;
`pass ms` is only the recorded rewrite span. Phase timings are inclusive and
must not be summed indiscriminately. Complete reports are required; the MIR
must be byte-for-byte deterministic within each mode. `--assert-rewrites`
also checks that the built-in pass changes matching workloads but not controls.

Outside the timed section, Cephyr compiles each variant to an object, the
host C driver links it, and the fixture checks its result. Use `--no-verify`
for a profile-only run or when cross-compiling. The benchmark does not measure
generated program runtime. MIR reductions are not necessarily native speedups;
verify and measure generated code separately before drawing performance
conclusions. Timing data are descriptive, not pass/fail thresholds.

## Recorded Prolific run

Release build on Linux AMD64 (Intel i7-11700K), pinned to CPU 6, with seven
samples after one warmup. All 22 fixtures compiled and passed native
self-checks in both modes. The initial integration verified the original
and final MIR on every QML invocation. The tuned path reads the lowerer's
canonical MIR directly, still verifies changed output, and adds more
integer identities to the built-in pass.

| Workload | Initial rewrite span | Tuned rewrite span | Tuned verifier span |
| --- | ---: | ---: | ---: |
| `dense-8x112` | 1.788 ms | 0.737 ms | 0.950 ms |
| `sparse-8x112` | 1.641 ms | 0.553 ms | 0.886 ms |
| `inert-8x112` | 1.664 ms | 0.523 ms | not needed |

The dense fixture went from 1,792 ALU operations to zero and from 200,224 to
180,128 object bytes. The inert fixture's MIR and object size were unchanged;
its initial post-rewrite verification alone took 0.977 ms. These are
observed medians, not guarantees about compilation or runtime speed.

With both passes selected, all four `strength-*` fixtures passed their
native checks. `strength-8x112` replaced all 896 MIR multiplications with
additions. Its total ALU count stayed at 1,792, while the object fell from
200,608 to 199,712 bytes.
