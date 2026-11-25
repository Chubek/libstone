# Cephyr / Blackstone — C11 optimization fixtures

For the two additional native AMD64 constant-materialization fixtures, see
[SandSLAB results and reproduction commands](../docs/SandSLAB.md).

The original Blackstone suite has **20 standalone C11 programs**, each with an independent `main` that returns zero if its known-answer tests pass. It targets Cephyr's **LP64** C model (64-bit `unsigned long`) and uses no libc functions, inline assembly, compiler intrinsics or undefined signed overflow. Every program must be compiled separately because each has its own `main`.

The goal is to measure potential Blackstone integration against Cephyr's baseline and to catch **wrong-code bugs**. Blackstone is **not yet integrated into Cephyr** in the repository version inspected; these fixtures are intended for the future adapter, and can already compare existing QML passes.

## Fixture coverage

| Program | Category / target | Main expected opportunities |
|---|---|---|
| `algebraic.c` | local identities | add-zero, multiply-one, XOR-self |
| `constant_fold.c` | local folding | integer constant folding |
| `copy_propagation.c` | SSA propagation | copies, constant propagation |
| `cse_local.c` | local CSE | duplicate pure ALU operations |
| `cse_dominance.c` | cross-block CSE | dominance and CFG |
| `bitwise_factor.c` | extended patterns | factoring, absorption, cancellation |
| `reassociation.c` | reassociation | modular integer constant grouping |
| `phi_join.c` | SSA joins | block parameters / phi interoperability |
| `loop_induction.c` | loops | induction-based strength reduction |
| `loop_multilatch.c` | loops | recurrence across multiple loop backedges |
| `unsigned_rem_mask.c` | QML overlap | guarded unsigned modulo and bit masks |
| `unsigned_wrap.c` | semantic boundary | modulo-2^64 wraparound |
| `signed_near_miss.c` | negative control | signed division/remainder vs. unsigned simplification |
| `dynamic_divisor.c` | negative control | dynamic nonzero divisor, mask near-miss |
| `memory_effects.c` | effects / safety | volatile access and effectful calls |
| `pointer_alias.c` | effects / safety | aliased read/store/read sequence |
| `shift_near_miss.c` | negative control | runtime-dependent but valid shift amounts |
| `stress_dense.c` | scale | 4 × 64 identity-heavy steps |
| `stress_sparse.c` | scale | 4 × 64 steps with sparse identities |
| `stress_inert.c` | scale | 4 × 64 steps with no intentional identity matches |

The program names describe the **intended pressure point**, not an assertion that the current Cephyr lowerer emits a particular kernel pattern or that a future Blackstone adapter can legally consume it. Inspect emitted MIR before claiming a kernel match. All expected values are generated from independent Python arithmetic in `tools/generate.py` and materialized in the C sources.

## Run tests with Cephyr

From a LibStone checkout, put this package alongside the repository or update the fixture paths below. Assuming Cephyr is already built:

```sh
cephyr=./build-cephyr/cephyr
"$cephyr" --no-factir cephyr-blackstone-c11/fixtures/algebraic.c -o /tmp/algebraic
/tmp/algebraic  # exit status 0 means pass
```

Each C file is also directly compilable with a conventional LP64 C11 compiler:

```sh
cc -std=c11 -O0 cephyr-blackstone-c11/fixtures/pointer_alias.c -o /tmp/pointer_alias
/tmp/pointer_alias
```

## Compare baseline and candidate

The included Python 3 benchmark runner compiles baseline and candidate MIR several times, checks deterministic output, records opcode counts, produces object files, and **builds and executes both versions** unless you request `--skip-run`. It emits a JSON summary plus the final MIR and object for each program/mode.

Use an *existing* QML pass as a demonstration:

```sh
python3 cephyr-blackstone-c11/benchmark.py \
  --compiler ./build-cephyr/cephyr \
  --output /tmp/cephyr-compare-01 \
  --target x86_64-linux-gnu \
  --candidate-arg=--qml-pass=canonicalize \
  --repeat 7 --warmup 2
```

Use a QML pipeline:

```sh
python3 cephyr-blackstone-c11/benchmark.py \
  --compiler ./build-cephyr/cephyr \
  --output /tmp/cephyr-compare-02 \
  --candidate-arg=--qml-pass=canonicalize \
  --candidate-arg=--qml-pass=strength_reduce \
  --candidate-arg=--qml-pass=integer_reduce
```

Once a Blackstone adapter exposes its own Cephyr driver flags, pass those through the same `--candidate-arg=...` mechanism. The runner does **not** assume or invent Blackstone CLI options.

Use `--fixture '^(cse_|phi_|loop_)'` to select a group. For a target that cannot execute natively, pass `--runner /path/to/qemu-riscv64` (or equivalent), or `--skip-run` for compile-only measurements. Pass `--with-factir` if you want FactIR present in *both* configurations; by default it is off to isolate the selected rewrites. Use `--objdump /path/to/llvm-objdump` for optional `.text` byte counts (object bytes are always recorded).

To isolate the Sea-of-Nodes optimizer, disable FactIR (the runner default) and Blackstone in both modes:

```sh
python3 cornerstone/Cephyr/benchmark/benchmark.py \
  --compiler build/cephyr/cephyr --output build/cephyr-mir-bench \
  --no-blackstone --baseline-arg=--no-mir-opt --candidate-arg=--mir-opt \
  --repeat 11 --warmup 2
```

Baseline and candidate samples alternate within each fixture. Use `--fixture '^stress_'` for a fast iteration on dense, sparse, and inert workloads, then run the full suite before accepting a change. To measure a compiler change rather than a flag, pass the previous executable as `--baseline-compiler`.

**Interpretation:** The runner's time measurements are MIR compiler invocations (including process startup), not runtime performance of the produced binaries. Object-size deltas are not necessarily instruction-count or `.text`-size deltas. A changed MIR does not establish a speedup. The self-check cases establish only sampled functional equality, not formal equivalence. An optimizer may pass all samples while still being wrong for other inputs.

## Regenerate / audit the expected results

```sh
python3 cephyr-blackstone-c11/tools/generate.py
```

This rewrites the fixtures and `manifest.json` deterministically. The generator uses Python integers masked to 64 bits and an explicit truncation-toward-zero model for signed division/remainder. Review its oracle functions independently when expanding the suite.

## Suggested CI matrix

1. Compile and run every program with a reference C11 compiler at `-O0` and `-O2`.
2. Compile and run every program with Cephyr at baseline, QML-only, Blackstone-only, and combined configurations when supported.
3. Execute native AMD64 and AArch64/RV64 under the appropriate emulator.
4. Reject failed self-checks, malformed MIR, or nondeterministic MIR; record but do not fail solely on code-size/timing regressions.
5. Review negative controls in particular: signed remainder, aliasing, volatile accesses, calls and multiple loop latches.
