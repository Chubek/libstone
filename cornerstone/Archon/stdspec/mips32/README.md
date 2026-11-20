# MIPS32 integer subset

Compile `root.arch` to load this little-endian MIPS32 (MIPSel) subset. It
describes 32-bit R-format `ADDU`, `SUBU`, `AND`, `OR`, `XOR`, `NOR`, `SLL`,
`SRL`, and `SRA`, plus I-format `ADDIU`, `ANDI`, `ORI`, `XORI`, and `LUI`.
Addition and subtraction wrap modulo 2³² without trapping on overflow.
Despite its name, `ADDIU` sign-extends its 16-bit immediate before addition;
the source range is −32768 through 32767. `ANDI`, `ORI`, and `XORI`
zero-extend their unsigned 16-bit immediates, and `LUI` places its unsigned
immediate in bits 31:16 with the low half cleared. Shift counts are immediate
values from 0 to 31; only `SRA` replicates the sign bit.

Archon uses `r0`–`r31`, with `zero` as an alias for `r0`. Reads of register
zero return zero and writes to it are discarded. Common MIPS assemblers
instead use `$0`–`$31` or ABI names such as `$zero` and `$a0`. Those spellings
are not part of these Archon assembly templates.

## Golden encodings

See also the machine-readable [golden vectors](encoding-golden.tsv).

| Archon assembly | Machine word | Little-endian bytes |
| --- | --- | --- |
| `addu r1, r2, r3` | `0x00430821` | `21 08 43 00` |
| `subu r4, r5, r6` | `0x00A62023` | `23 20 A6 00` |
| `and r7, r8, r9` | `0x01093824` | `24 38 09 01` |
| `or r10, r11, r12` | `0x016C5025` | `25 50 6C 01` |
| `xor r13, r14, r15` | `0x01CF6826` | `26 68 CF 01` |
| `addiu r16, r17, -7` | `0x2630FFF9` | `F9 FF 30 26` |
| `addiu r2, r0, -32768` | `0x24028000` | `00 80 02 24` |
| `addiu r3, r4, 32767` | `0x24837FFF` | `FF 7F 83 24` |
| `addu r0, r1, r2` | `0x00220021` | `21 00 22 00` |
| `nor r5, r6, r7` | `0x00C72827` | `27 28 C7 00` |
| `andi r8, r9, 65535` | `0x3128FFFF` | `FF FF 28 31` |
| `ori r10, r0, 32768` | `0x340A8000` | `00 80 0A 34` |
| `xori r11, r12, 32769` | `0x398B8001` | `01 80 8B 39` |
| `lui r13, 65535` | `0x3C0DFFFF` | `FF FF 0D 3C` |
| `sll r14, r15, 31` | `0x000F77C0` | `C0 77 0F 00` |
| `srl r16, r17, 0` | `0x00118002` | `02 80 11 00` |
| `sra r18, r19, 31` | `0x001397C3` | `C3 97 13 00` |

To cross-check with `llvm-mc -triple=mipsel -mcpu=mips32 -show-encoding`,
write registers as `$` followed by their number, e.g. `addu $1, $2, $3`.
Use `.set noat` to disable the assembler's warnings about using `$1`.
External tools can print `sll $0, $0, 0` as `nop`; Archon accepts only the
underlying instruction. Run the local tests from `cornerstone/Archon/` with
`python3 -m unittest discover -s stdspec/mips32`.

## Limits

This is not a complete MIPS32 release profile. It excludes trapping `ADD`,
`ADDI`, and `SUB`, comparisons, register-count shifts, rotates,
multiply/divide, the big-endian variant, and compressed encodings. The
semantics model GPR updates only, not shift-encoding execution hints, the PC,
branches or delay slots, memory, exceptions, coprocessors, or the ABI.
