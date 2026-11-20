# CLR / ECMA-335 CIL opcode subset

`root.arch` assembles and disassembles selected Common Intermediate
Language (CIL) instructions from ECMA-335, 6th edition, using the
standard lower-case mnemonics. Every supported instruction is one byte, with
no explicit operands. Evaluation-stack operands are implicit, as are the
values in `ldnull` and `ldc.i4.m1` through `ldc.i4.8`.

## Coverage

| Opcode bytes (hex) | Instructions | Count |
| --- | --- | ---: |
| `00`, `14`–`1e`, `25`–`26` | `nop`, null and small integer constants, `dup`, `pop` | 14 |
| `58`–`66` | Arithmetic, bitwise operations and shifts | 15 |
| `67`–`6e`, `76`, `d1`–`d3`, `e0` | Numeric conversions | 13 |
| `82`–`8b`, `b3`–`ba`, `d4`–`d5` | Overflow-checking numeric conversions | 20 |
| `d6`–`db` | Overflow-checking arithmetic | 6 |

The [golden vectors](encoding-golden.tsv) list all 68 encodings. There are
no selectable features in this subset.

## Execution boundary

The architecture has no register file. Its bounded evaluation state uses one
64-bit value slot plus an 8-bit kind tag (`int32` = 1, `int64` = 2,
native int = 3, `F` = 4, object reference = 5).

| Opcode bytes | Executable | Encoding-only |
| --- | ---: | ---: |
| `00`, `14`–`1e`, `25`–`26` | `nop`, null/small-int constants, `dup`, `pop` (14) | 0 |
| `58`–`66` | 0 | Arithmetic, bitwise operations, and shifts (15) |
| `67`–`6e`, `76`, `d1`–`d3`, `e0` | 0 | Numeric conversions (13) |
| `82`–`8b`, `b3`–`ba`, `d4`–`d5` | 0 | Checked conversions (20) |
| `d6`–`db` | 0 | Checked arithmetic (6) |
| **Total** | **14** | **54** |

`ldnull` pushes the model's zero reference payload with object-reference tag
5. The small integer constants push their low 32-bit bit patterns with tag 1.
`dup` and `pop` copy or remove a complete modeled value/tag pair independent
of its kind. Every paired update is transactional, including failures after
the first stack has already changed in the working state.

CIL numeric opcodes are overloaded across stack types. Current straight-line
ArchSem cannot inspect kind tags, dispatch by native width, represent
floating-point semantics, or raise division and overflow traps. Even the
unchecked integer cases therefore remain encoding-only rather than exposing
an inaccurate fixed-type projection.

From `cornerstone/Archon`:

```sh
python3 -m archon check stdspec/clr/root.arch
python3 -m archon compile stdspec/clr/root.arch -o cil.archdb
python3 -m archon asm cil.archdb ldc.i4.m1  # 15
python3 -m archon dis cil.archdb d6         # add.ovf
python3 -m unittest discover -s stdspec/clr -v
```

`asm-program` and `dis-program` handle sequences consisting entirely of
these opcodes. For example, `ldc.i4.2`, `ldc.i4.3`, `add.ovf`, `pop` on
separate lines encodes as `18 19 d6 26`. The tests exercise the serialized
ArchDB, pin and round-trip all golden bytes, reject every other opcode byte,
check the exact executable set, and cover successful and rolled-back stack
updates.

## ABI coverage

This entry point does not emit `abi_facts`. The [CLI virtual calling convention][cil-calls]
takes values from the caller's evaluation stack, exposes them as callee argument
slots, and passes any result back on the caller's evaluation stack. The subset
does not model managed method calls, argument slots, or `ret`. Its value and kind
stacks do not supply the register file, stack pointer, or return-address register
required by `abi_facts` v1. Platform-specific unmanaged interop is a separate
boundary and is not described by this bytecode subset.

[cil-calls]: https://carlwa.com/ecma-335/#i.12.4.1.4-virtual-calling-convention.md

## References and limits

All 68 mnemonic/byte pairs were checked against the
[ECMA-335](https://ecma-international.org/publications-and-standards/standards/ecma-335/)
[Partition III §1.2.1 opcode table](https://carlwa.com/ecma-335/#iii.1.2.1-opcode-encodings.md)
and matched single-byte, `InlineNone`
entries in [.NET 10.0.0 CoreCLR's opcode table](https://github.com/dotnet/runtime/blob/v10.0.0/src/coreclr/inc/opcode.def).
All 68 also decoded with dncil 1.0.2, consuming exactly one byte and
reporting no explicit operands.

This is a raw opcode codec with limited stack execution, not an ILAsm
implementation, CLR interpreter, or verifier. The Archon address width is
metadata only; it does not fix the width of CIL native integers.

Multi-byte instructions are unsupported, including `ldc.i4.s`,
`ldc.i4`, `ceq` (`fe 01`), and prefixes such as `volatile.` (`fe 13`).
Single-byte instructions outside the listed subset (for example `ldarg.0`
and `ret`) are also unsupported. The codec does not read, write, or validate
PE/CLI files, assemblies, modules, metadata, or method headers. Pass only
opcode bytes starting at a known instruction boundary, not a DLL, EXE,
or an arbitrary slice.
