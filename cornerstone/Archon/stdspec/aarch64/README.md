# AArch64 integer subset

Compile `root.arch` to load this little-endian AArch64 subset. It describes
`ADD`, `SUB`, `AND`, `ORR`, `EOR`, `BIC`, `ORN`, and `EON` in their 64-bit,
unshifted, three-register forms, plus the four-register `MADD` and `MSUB`.
Every instruction is 32 bits wide and leaves NZCV unchanged. Arithmetic
wraps modulo 2⁶⁴.

The three-register instructions use the shifted-register form with `LSL #0`
required. The assembly templates omit the shift. `BIC`, `ORN`, and `EON`
complement the second source before applying AND, OR, or XOR. `MADD` adds
the low 64 bits of the product `Rn × Rm` to `Ra`; `MSUB` subtracts that product
from `Ra`. Register encoding 31 is `xzr` in every operand position: reads
return zero and writes are discarded. The specification uses `x[31]` as its
internal slot for `xzr`; this is not SP.

## Golden encodings

See also the machine-readable [golden vectors](encoding-golden.tsv).

| Assembly | Machine word | Little-endian bytes |
| --- | --- | --- |
| `add x1, x2, x3` | `0x8B030041` | `41 00 03 8B` |
| `sub x4, x5, x6` | `0xCB0600A4` | `A4 00 06 CB` |
| `and x7, x8, x9` | `0x8A090107` | `07 01 09 8A` |
| `orr x10, x11, x12` | `0xAA0C016A` | `6A 01 0C AA` |
| `eor x13, x14, x15` | `0xCA0F01CD` | `CD 01 0F CA` |
| `add xzr, x1, x2` | `0x8B02003F` | `3F 00 02 8B` |
| `sub x3, xzr, x4` | `0xCB0403E3` | `E3 03 04 CB` |
| `orr x5, xzr, x6` | `0xAA0603E5` | `E5 03 06 AA` |
| `bic x16, x17, x18` | `0x8A320230` | `30 02 32 8A` |
| `orn x19, x20, x21` | `0xAA350293` | `93 02 35 AA` |
| `eon x22, x23, x24` | `0xCA3802F6` | `F6 02 38 CA` |
| `madd x25, x26, x27, x28` | `0x9B1B7359` | `59 73 1B 9B` |
| `msub x29, x30, xzr, x1` | `0x9B1F87DD` | `DD 87 1F 9B` |

The bytes can be checked with `llvm-mc -triple=aarch64 -show-encoding`.
External tools may print `sub x3, xzr, x4` as `neg x3, x4`,
`orr x5, xzr, x6` as `mov x5, x6`, or a `madd` with `Ra=xzr` as `mul`.
These aliases are not accepted by Archon. Run the local tests from
`cornerstone/Archon/` with `python3 -m unittest discover -s stdspec/aarch64`.

## Limits

This is not a complete AArch64 implementation. No W-register, stack-pointer,
immediate, extended-register, nonzero-shift, widening-multiply, or flag-setting
forms are modeled. Explicit shift syntax is not defined. The semantics model
GPR updates only, not NZCV, the PC, branches, memory, exceptions, or ABI.
