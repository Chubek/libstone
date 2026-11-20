# RISC-V base integer ALU subsets

Use `rv32i.arch` for XLEN=32 or `rv64i.arch` for XLEN=64. Both entry
files import `base.arch`, which defines the `I`-gated instructions, 32 integer
registers with standard ABI names, and three 32-bit instruction formats.

| Format | Instructions |
| --- | --- |
| R-type (`OP`, `0x33`) | `add`, `sub`, `xor`, `or`, `and`, `sll`, `srl`, `sra` |
| I-type (`OP-IMM`, `0x13`) | `addi`, `xori`, `ori`, `andi` |
| U-type (`LUI`, `0x37`) | `lui` |

The two targets encode these instructions identically. Arithmetic wraps modulo
2^XLEN. I-type immediates are signed 12-bit values (`-2048` through `2047`)
and are sign-extended to XLEN, including for the logical operations. `lui`
accepts an unsigned 20-bit field, shifts it left by 12 bits, and sign-extends
the resulting 32-bit pattern on RV64. The register shifts use the low five
(RV32) or six (RV64) bits of the count; only `sra` replicates the sign bit.
Reads of `x0` return zero and writes to it are discarded. For example, adding
one to `0xffffffff` yields `0` on RV32 and `0x100000000` on RV64.

These are **not complete RV32I or RV64I implementations**. The subset omits
immediate shifts, comparisons, `auipc`, RV64 `*W` operations, loads and stores,
control flow (including PC updates), fences, system instructions, and assembly
pseudo-instructions.

## Legacy integer ABI facts

`rv32i.arch` publishes ILP32 facts with 4-byte stack argument slots, and
`rv64i.arch` publishes LP64 facts with 8-byte slots. Both follow the
[RISC-V psABI integer register convention] and [integer calling convention]
for fixed-arity calls with one-word integer or pointer arguments and results:

- The first eight arguments use `a0`–`a7` (`x10`–`x17`), in order. A
  single-register result uses `a0`; `ra` (`x1`) holds the return address.
- The stack grows downward and is aligned to 16 bytes. The first stack
  argument is at offset zero from `sp` (`x2`) on callee entry.
- `ra`, `a0`–`a7`, and `t0`–`t6` are caller-saved. `sp` and `s0`–`s11`
  are preserved. `zero`, `sp`, `gp`, and `tp` are not allocatable.

These are scalar facts in the ArchDB `abi_facts` record, not a full psABI
classifier. They do not describe narrower-scalar extensions, multi-register
arguments or returns, aggregates, variadic calls, or floating-point calling
conventions. They do not change the legacy instruction subsets or qualify
either target for the native backend.

[RISC-V psABI integer register convention]: https://riscv-non-isa.github.io/riscv-elf-psabi-doc/#_integer_register_convention
[integer calling convention]: https://riscv-non-isa.github.io/riscv-elf-psabi-doc/#integer-cc

## Encoding goldens

`encoding-golden.tsv` contains literal little-endian instruction bytes followed
by an assembly spelling. Its 18 vectors cover all 13 instructions, both
signed immediate endpoints, a write to `x0`, and both aliases for `x8`.
Run the Archon encoding, round-trip, and register-effect tests from
`cornerstone/Archon/`:

```sh
python3 -m unittest discover -s stdspec/riscv
```

To check the goldens independently with LLVM's assembler for both `riscv32`
and `riscv64`, run from this directory in Bash:

```bash
set -o pipefail
for target in riscv32 riscv64; do
    tail -n +2 encoding-golden.tsv | cut -f2 |
        llvm-mc -triple="$target" -show-encoding |
        sed -n 's/.*encoding: \[\(.*\)\]/\1/p' |
        sed 's/0x//g; s/[,[:space:]]//g' |
        diff - <(tail -n +2 encoding-golden.tsv | cut -f1) || exit 1
done
```

The LLVM comparison checks encoding only; neither suite establishes full
ISA conformance.

## RV64IM native profile

`rv64im.arch` is a separate entry point for the first native compiler backend.
It adds the selected M operations, comparisons, immediate left shift and
encodable control and memory forms; it does not claim full ISA or emulator
coverage. ArchDB v1 has no scattered instruction fields, so the native
provider supplies reviewed forms for stores and branches and delegates final
encoding and relocations to an assembler. See [Embedding](../../Embedding.md)
and [implementation boundaries](../../IMPLEMENTATION.md).
