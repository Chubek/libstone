# ArchCollection

Real, deliberately small instruction subsets from `.info/binutils` (2011).
Each `NAME/NAME.archw` produces `NAME.h` and `NAME.c` using
[ArchWeave](../Weavers/ArchWeave/README.md).

| Profile | BFD family / machine | Forms |
| --- | --- | --- |
| MIPS32 | `mips` / 32 (`mips:isa32`) | addu, subu, addiu, ori, lw, sw, beq, bne, lui, sll, j, jal, jr |
| PowerPC32 | `powerpc` / 32 | addi, addis, ori, lwz, stw, add, subf, or, b, bl, blr |
| SPARC32 | `sparc` / 1 | add.r/.i, sub.r/.i, or.r/.i, ld.i, st.i, jmpl.i, sethi, call, ba |

All are four-byte encodings. Byte APIs support explicit big/little order;
the profiles default to big endian. SPARC suffixes distinguish register and
immediate encodings, **not GNU assembler spelling**. Operand ordering is the
generated metadata order, not necessarily assembly syntax (e.g. loads expose
register/base/displacement). No pseudo-instructions, feature extensions,
execution semantics or complete ISA coverage are implied.

MIPS branches use signed byte offsets from PC+4; j/jal operands contain only
the low 28 address bits. PowerPC b/bl and SPARC ba/call offsets are relative to
the instruction PC. `sethi` exposes the raw 22-bit immediate, not its shifted
runtime value. No PC or delay-slot behavior is simulated.

```c
#include "ArchCollection/MIPS32/MIPS32.h"
int main(void) {
    const int64_t operands[] = { 8, 0, -1 }; /* addiu rt, rs, imm */
    uint32_t word;
    MIPS32_decoded decoded;
    if (MIPS32_encode_word("addiu", operands, 3, &word)) return 1;
    if (word != UINT32_C(0x2408ffff)) return 2;
    return MIPS32_decode_word(word, &decoded);
}
```

Compile from `lapislazuli/` with
`cc -std=c99 -I. example.c ArchCollection/MIPS32/MIPS32.c -o example`.

```sh
lua5.4 ArchCollection/regenerate.lua --list
lua5.4 ArchCollection/regenerate.lua MIPS32
lua5.4 ArchCollection/regenerate.lua --check
python3 test_collections.py
```

To extend: add non-overlapping real encodings with citations, then add known
word vectors to `test_collections.py`, regenerate and run normal/UBSan tests.
Register all operand bits explicitly; unsupported encodings must remain errors.
