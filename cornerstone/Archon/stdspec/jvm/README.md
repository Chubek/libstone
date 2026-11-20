# JVM bytecode opcode subset

`root.arch` assembles and disassembles selected Java Virtual Machine
instructions using the lower-case mnemonics in the Java SE 21 JVM
Specification (JVMS). Every supported instruction is one byte, with no
explicit operands. Stack operands are implicit, as are the values in
instructions such as `iconst_m1` and `aconst_null`.

## Coverage

| Opcode bytes (hex) | Instructions | Count |
| --- | --- | ---: |
| `00`–`0f` | `nop`, null, and implicit integer, long, float and double constants | 16 |
| `57`–`5f` | Stack operations from `pop` through `swap` | 9 |
| `60`–`83` | Numeric arithmetic, integer shifts and bitwise operations | 36 |
| `85`–`93` | Numeric conversions | 15 |
| `94`–`98` | Long, float and double comparisons | 5 |

The [golden vectors](encoding-golden.tsv) list all 81 encodings. There are
no selectable features in this subset.

## Execution boundary

The architecture has no register file. Its bounded operand state uses one
64-bit value slot plus an 8-bit kind tag (`int` = 1, `long` = 2). An `int`
payload stores its 32-bit bit pattern in the low bits.

| Opcode bytes | Executable | Encoding-only |
| --- | ---: | ---: |
| `00` | `nop` (1) | 0 |
| `01`–`0f` | `iconst_m1`–`iconst_5`, `lconst_0`, `lconst_1` (9) | Reference and floating constants (6) |
| `57`–`98` in the covered ranges | 0 | Stack, numeric, conversion, and comparison entries (65) |
| **Total** | **10** | **71** |

The executable constants have fixed result kinds and need no input
preconditions. Their paired value/tag pushes are transactional. JVM stack
manipulation depends on category forms, while numeric instructions require
verified operand kinds; division and remainder can also trap. Floating-point,
conversion, and comparison behavior likewise exceeds current straight-line
ArchSem. Those instructions remain explicitly encoding-only rather than
executing against unchecked tags.

From `cornerstone/Archon`:

```sh
python3 -m archon check stdspec/jvm/root.arch
python3 -m archon compile stdspec/jvm/root.arch -o jvm.archdb
python3 -m archon asm jvm.archdb iconst_m1  # 02
python3 -m archon dis jvm.archdb 60        # iadd
python3 -m unittest discover -s stdspec/jvm -v
```

`asm-program` and `dis-program` handle sequences consisting entirely of
these opcodes. For example, `iconst_2`, `iconst_3`, `iadd`, `pop` on
separate lines encodes as `05 06 60 57`. The tests exercise the serialized
ArchDB, pin and round-trip all golden bytes, reject every other opcode byte,
check the exact executable set, and cover successful and rolled-back stack
updates.

## ABI coverage

This entry point does not emit `abi_facts`. In [JVMS §2.6][jvms-frames],
invocation takes arguments from the caller's operand stack and places them in
the new frame's local variables; on normal return a result, if any, is pushed
onto the caller's operand stack. The subset does not model method invocation,
frames, or returns. Its operand and kind stacks do not supply the register file,
stack pointer, or return-address register required by `abi_facts` v1. Native
method interfaces are outside this bytecode subset.

[jvms-frames]: https://docs.oracle.com/javase/specs/jvms/se21/html/jvms-2.html#jvms-2.6

## References and limits

See [JVMS §6.5](https://docs.oracle.com/javase/specs/jvms/se21/html/jvms-6.html#jvms-6.5)
for instruction formats and [Chapter 7](https://docs.oracle.com/javase/specs/jvms/se21/html/jvms-7.html)
for the opcode index. All 81 mnemonic/byte pairs were cross-checked against
the [Java Version Almanac opcode index](https://javaalmanac.io/bytecode/opcodes/)
and the [ASM opcode table bundled with OpenJDK 21.0.2](https://github.com/openjdk/jdk21u/blob/jdk-21.0.2-ga/src/java.base/share/classes/jdk/internal/org/objectweb/asm/Opcodes.java).
All 81 also assembled and disassembled with Jawa 2.2.0, consuming exactly
one byte and reporting no explicit operands.

This is a raw opcode codec with limited constant execution, not a JVM
interpreter or verifier. The Archon address width is metadata only; it does
not describe a JVM reference size. The modeled stacks do not implement
class-file `max_stack` accounting, local variables, or return-address values.

Multi-byte instructions (including `bipush`, `ldc`, `iinc`, `wide`, and
switches) are unsupported. So are instructions outside the listed subset,
even when they have one-byte encodings (for example `iload_0` and `return`).
The codec does not read, write, or validate class files, `Code` attributes,
constant pools, stack maps, JARs, or Java modules. Pass only opcode bytes
starting at a known instruction boundary, not an arbitrary file or slice.
