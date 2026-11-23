# SandSLAB: shorten AMD64 immediate loads

Use `--sandslab` to enable the late pass and `--no-sandslab` to disable it.
The default is off. `[sandslab] enabled = true` in Cephyr TOML sets a default;
an explicit CLI flag wins. The setting is independent of Blackstone, QML and
FactIR. A Moonstone-enabled build exposes the `sandslab` stage in its profile.

The native AMD64 backend materializes constants as `movabsq $0x…, %rax`
(10 bytes). For that exact instruction, the selector uses:

| Full 64-bit value | SandSLAB form | Bytes |
|---|---|---:|
| `0..0xffffffff` | `mov_eax_imm32` (zero-extend EAX) | 5 |
| `0xffffffff80000000..0xffffffffffffffff` | `mov_rax_sext32` (sign-extend imm32) | 7 |
| Other | original `movabsq` | 10 |

Both replacements preserve all flags and write exactly the original RAX value.
They do not access memory, change the stack, introduce a scratch register, or
require optional CPU features. `Sandstone::sandslab` is a private, header-only
build dependency. The build embeds just the selected forms from `sandstone/isa/x86.isa`
using Sandstone's catalog reader. `sandslab::Program::emit` and `compile`
validate and encode each replacement. The driver inserts the resulting bytes
as a GAS `.byte` directive; the assembler still owns label layout, branch
relaxation and relocations. Symbolic operands and noncanonical lines are never
matched. On an ISA error the entire original assembly is retained.

This runs on C lowered by the recognized native AMD64 provider, after native
emission and before appending data. User assembly, MIR output, other targets
and SDK backends bypass it. Installed binaries need no loose ISA file.

## Reproduce

From the repository root on native Linux x86-64 (choose a new results directory):

```sh
B=/mnt/nvme/opencode-builds/cephyr-sandslab
mkdir -p "$B/tmp"
export TMPDIR="$B/tmp"
cmake -S cornerstone/Cephyr -B "$B" -DCMAKE_BUILD_TYPE=Release
cmake --build "$B" --target cephyr -j4
ctest --test-dir "$B" -R '^cephyr.sandslab$' --output-on-failure
bench=cornerstone/Cephyr/benchmark
taskset -c 1 python3 "$bench/benchmark.py" --compiler "$B/cephyr" \
  --output "$B/results" --target x86_64-linux-gnu --fixture '^sandslab_' \
  --baseline-arg=--no-sandslab --candidate-arg=--sandslab \
  --objdump /usr/bin/objdump --repeat 5 --warmup 2
taskset -c 1 python3 "$bench/tools/time_sandslab.py" "$B/results"
```

Native measurements (same Release compiler, CPU 1, two warmups, 21 alternating
pairs for `-c`, warm backend-source cache):

| Fixture | `.text`, off → on | `-c` median ms, off → on | Median paired delta |
|---|---:|---:|---:|
| `sandslab_literals` | 4683 → 4435 (−5.3%) | 416.27 → 425.12 | +3.93 ms |
| `sandslab_wide` | 2635 → 2467 (−6.4%) | 420.38 → 422.05 | −3.47 ms |

Each fixture passed its native self-checks in both modes. The wide fixture
contains both ineligible literals and shortenable constants in the checks.
MIR was identical: the runner's `--emit-mir` timings **do not measure this
pass**. The object timings include startup, backend build and assembly, and
their opposite-signed deltas should not be interpreted as a compile speedup.
No application runtime speedup was measured.

An initial implementation parsed the complete catalog on first use. Timing
only the pass on the same baseline assembly, with full vs. projected catalog,
reduced first invocation from 835 to 120 µs (literals) and 875 to 94 µs
(wide). Warm projected medians over 101 invocations were 74 and 50 µs.
