# AMD64 classic register subset

Compile `root.arch` for four runnable long-mode instructions: `MOV` copies a
64-bit register, `NOT` complements all 64 bits, `BSWAP` reverses the eight bytes,
and `XCHG` swaps two registers using their original values (a self-exchange is a
no-op). None changes RFLAGS.

Every instruction is exactly three bytes. The first byte is always `48`
(`REX.W=1`, `R=X=B=0`). For `MOV`, `NOT`, and `XCHG`, ModR/M uses `mod=11b`
(register-direct). `MOV` uses opcode `89` (`r/m64` destination first, `r64`
source second). `NOT` uses `F7 /2`. `XCHG` uses `87`, with the first register in
`r/m` and the second in `reg`. `BSWAP` has no ModR/M byte; it uses `0F C8+r`.

Assembly uses Intel operand order and lowercase register aliases: `rax`, `rcx`,
`rdx`, `rbx`, `rsp`, `rbp`, `rsi`, `rdi`, in encoding order. These aliases are
accepted as input; Archon disassembly and execution output use the internal
names `gpr0` through `gpr7`. The numbered names are accepted by Archon but are
not AMD64 assembly syntax.

From `cornerstone/Archon`:

```sh
python3 -m archon compile stdspec/amd64/root.arch -o amd64.archdb
python3 -m archon asm amd64.archdb 'mov rax, rbx'  # 4889d8
python3 -m archon dis amd64.archdb 4889d8          # mov gpr0, gpr3
python3 -m archon run amd64.archdb 'bswap rax' --reg rax=0x0102030405060708
# gpr0 becomes 0x0807060504030201 (printed in decimal)
python3 -m unittest discover -s stdspec/amd64 -v
```

## Golden encodings

The [golden vectors](encoding-golden.tsv) use GNU `as --64` with
`.intel_syntax noprefix`. For example:

| Intel assembly | Bytes |
| --- | --- |
| `mov rax, rbx` | `48 89 D8` |
| `not rdx` | `48 F7 D2` |
| `bswap rdi` | `48 0F CF` |
| `xchg rcx, rdx` | `48 87 D1` |

To check the vectors independently, pipe their assembly into GNU `as` and
inspect the resulting bytes:

```sh
awk -F '\t' 'BEGIN { print ".intel_syntax noprefix\n.text" } !/^#/ && NF { print $2 }' \
    stdspec/amd64/encoding-golden.tsv | as --64 -o amd64-golden.o
objdump -d -Mintel amd64-golden.o
```

Assemblers can reverse the register fields for `XCHG` without changing its behavior.
They can also choose the accumulator short form or encode `xchg rax, rax` as
`nop`; Archon always emits this subset's `48 87 /r` form.

## Limits

Only the eight classic 64-bit GPRs and the listed encodings are modeled. There
are no other REX combinations, register widths or aliases (such as `eax`),
alternate `MOV` encodings, immediates, memory forms, other prefixes, or
variable-length decoding. The register state does not include RFLAGS, RIP,
memory, faults, or the ABI. In particular, the exchange semantics are only for
registers; this spec does not model the locking behavior of memory `XCHG`.
`MOVSXD r64, r/m32` is not included: its register-direct form reads a
32-bit register name and writes a 64-bit register name, which the current
Archon register operands cannot distinguish. Flag-writing ALU operations and
stack or control-transfer operations are not included as runnable instructions.
