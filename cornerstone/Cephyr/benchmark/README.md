# Cephyr rewrite benchmark

Build a Release compiler with `-DCEPHYR_ENABLE_MOONSTONE=ON`. Run the
benchmark on Linux with Cephyr's target assembler/linker and `llvm-objdump`
available:

```sh
python3 cornerstone/Cephyr/benchmark/run.py \
  --cephyr build-cephyr/cephyr --output /path/to/new/cephyr-results \
  --repeat 7 --assert-rewrites
```

The default suite compares the baseline (no QML) with the built-in
`canonicalize` pass. Its six handwritten and sixteen deterministic generated
cases exercise joins, loops, integer promotions, memory and calls, varying
canonicalization match density, and multiplication by two. Use
`--fixture 'dense|inert'` to select names by regular expression.

### Guarded integer reduction

`--suite integer-reduce` runs six pipelines in one comparison: baseline,
`canonicalize`, `strength_reduce`, the two together in that order,
`integer_reduce` alone, and all three in that order. It includes unsigned
remainder by powers of two (including 1 and `1UL << 63`), redundant masks,
and a mixed workload where all three passes apply. Near-misses use odd
non-power-of-two and input-dependent divisors, signed remainder, and masks
that actually clear bits. No divisor is zero. Each workload has four kernels
with 48 opportunities or near-misses each, and a separate driver checks six
seed or boundary inputs per kernel against an independent Python LP64 model.
Only the kernel objects contribute to the code metrics for this suite.

For example, from an AMD64 host (each destination must be new):

```sh
bench=cornerstone/Cephyr/benchmark/run.py
cephyr=build-cephyr/cephyr
qemu_bin=/path/to/qemu/bin
taskset -c 6 python3 -B "$bench" --cephyr "$cephyr" \
  --output /path/to/new/amd64 --target x86_64-linux-gnu \
  --suite integer-reduce --repeat 12 --warmup 2 --assert-rewrites
taskset -c 6 python3 -B "$bench" --cephyr "$cephyr" \
  --output /path/to/new/aarch64 --target aarch64-linux-gnu \
  --runner "$qemu_bin/qemu-aarch64-static" \
  --suite integer-reduce --repeat 12 --warmup 2 --assert-rewrites
taskset -c 6 python3 -B "$bench" --cephyr "$cephyr" \
  --output /path/to/new/rv64 --target riscv64-linux-gnu \
  --runner "$qemu_bin/qemu-riscv64-static" \
  --suite integer-reduce --repeat 12 --warmup 2 --assert-rewrites
```

Choose an available CPU in place of `6`; run targets serially for timing.
To compare a named profile instead of the suite's built-in candidate
pipelines, supply candidate-only arguments and a config with no active
default profile:

```sh
python3 cornerstone/Cephyr/benchmark/run.py \
  --cephyr build-cephyr/cephyr --output /path/to/new/profile-results \
  --config path/to/Cephyr.toml --candidate-arg=--profile=my_profile
```

### Reading results

For each workload the runner measures `--emit-mir --no-cache` with Prolific.
It rotates mode order each round and discards warmups. `MIR ms` is elapsed
compiler invocation time (including startup and trace writing); `QML ms` is
only the instrumented rewrite span. Inclusive phase timings must not be
summed. `summary.json` retains per-run samples, wall min/median/max, phase
medians, MIR bytes/opcode counts, and native object metrics. Per-run MIR and
Prolific traces are also retained. The runner requires byte-for-byte
deterministic MIR within each mode; `--assert-rewrites` checks the expected
MIR changes and unchanged controls for built-in pipelines. The integer
suite also asserts how many guarded `rem_u` or `and` operations disappear.

Outside the timed section, Cephyr produces a relocatable object for each
mode. The runner reports decoded `.text` instructions (including any decoded
padding), `.text` bytes including padding, and whole-object file bytes.
Deltas are against the same workload and target's baseline. Disassemblies
are retained. The runner links statically using a small Linux entry point,
without libc, and checks every mode by exit status. Results are labeled
`native`, `runner`, `compile-only` (no cross-target runner or `--no-verify`),
or `mir-only` (`--mir-only`, useful without target assembly/disassembly
tools). Failed compilation, linking, or execution is an error, not a skip.

Timings are descriptive, not pass/fail thresholds. They do not cover target
code generation, linking, or generated program runtime. Static instruction
counts are not dynamic counts or speed estimates; QEMU is used for result
verification, not timing.

## Recorded integer reduction run

Release Cephyr 0.1.0 with Prolific, built with GCC 16.2.1; target assembly,
linking, and disassembly used LLVM 22.1.8. The host was Linux 6.18.52 on a
Core i7-11700K, pinned to logical CPU 6. The commands above used 12 measured
repetitions after two warmups, with targets measured serially. All 48
workload/mode pairs per target passed their result checks: AMD64 natively,
AArch64 and RV64 under QEMU user-mode 11.1.3. The suite's MIR assertions
also passed.

Static deltas (instructions / `.text` bytes) for `integer_reduce` alone
relative to baseline:

| Workload (`ir-…-4x48`) | AMD64 | AArch64 | RV64 |
| --- | ---: | ---: | ---: |
| `remainder` | −960 / −2,877 | −384 / −1,536 | **+192 / +768** |
| `remainder-high` | −960 / −3,261 | −384 / −1,536 | **+192 / +768** |
| `remainder-one` | −1,344 / −4,026 | −576 / −2,304 | −384 / −1,536 |
| `mask` | −381 / −2,061 | −384 / −1,536 | −384 / −1,536 |
| `mixed` | −960 / −2,880 | −384 / −1,536 | **+192 / +768** |
| `remainder-near`, `signed-near`, `mask-near` (each) | 0 / 0 | 0 / 0 | 0 / 0 |

On RV64, removing 192 `remu` instructions still made these three workloads
larger: in `remainder`, the disassembly drops 192 `remu` and 192 `mv` but
adds 192 each of `li`, `slli`, and `srl`. This is a code-size regression,
not evidence of a runtime regression.

The other passes only changed the `mixed` fixture in this suite. Compared
with baseline, all mode deltas for `mixed` were:

| Pipeline | AMD64 | AArch64 | RV64 |
| --- | ---: | ---: | ---: |
| `canonicalize` | −384 / −2,109 | −384 / −1,536 | −384 / −1,536 |
| `strength_reduce` | 0 / −192 | 0 / 0 | 0 / 0 |
| `canonicalize` + `strength_reduce` | −384 / −2,301 | −384 / −1,536 | −384 / −1,536 |
| `integer_reduce` | −960 / −2,880 | −384 / −1,536 | **+192 / +768** |
| All three | −1,344 / −5,181 | −768 / −3,072 | −192 / −768 |

Outside `mixed`, selecting all three produced the same static deltas as
`integer_reduce` alone.

Representative compiler-only timings, in ms. Parentheses show observed
min–max over the 12 samples; the rewrite column is the instrumented median
for `integer_reduce`. Even the unchanged mask control incurred rewrite
work. Long tails and overlapping ranges limit conclusions about the
magnitude of compile-time overhead; the host was pinned, not isolated.

| Target / workload | Baseline MIR | `integer_reduce` MIR | Rewrite |
| --- | ---: | ---: | ---: |
| AMD64 `remainder` | 4.07 (3.81–4.51) | 4.67 (4.51–5.12) | 0.25 |
| AMD64 `mask-near` | 3.81 (3.69–4.23) | 4.13 (4.02–4.45) | 0.23 |
| AArch64 `remainder` | 6.87 (5.23–8.80) | 7.66 (6.00–10.03) | 0.35 |
| AArch64 `mask-near` | 4.63 (4.45–18.07) | 5.20 (4.73–17.23) | 0.25 |
| RV64 `remainder` | 5.61 (5.42–10.39) | 6.47 (6.24–10.29) | 0.31 |
| RV64 `mask-near` | 4.42 (4.11–17.91) | 4.72 (4.42–14.79) | 0.25 |

## Historical canonicalization Prolific run

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
