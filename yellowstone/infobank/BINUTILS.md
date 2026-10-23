# Binutils opcode enrichment

`isa/*.isa` contains generated `binutils` fields in profiles and operations.
They reference the local **GNU binutils 2.22** tables, including their operand
codes, instruction forms, architecture/CPU flags, and opcode match/mask values.
Each variant identifies its source file, table, and line. Profile metadata
records SHA-256 source digests, coverage counts, and unmatched operations.

Regenerate from the repository root:

```sh
python3 yellowstone/scripts/enrich_isa_binutils.py
python3 yellowstone/scripts/enrich_isa_binutils.py --check
python3 -m unittest discover -s yellowstone/scripts -p test_enrich_isa_binutils.py
```

The importer needs Python 3.10+ and a C preprocessor (`cc` by default; select
another with `--cpp`). `--binutils` and `--isa-dir` override the input directories.
`--check` exits nonzero if generated metadata is stale, without writing files.
Only `BEGIN BINUTILS METADATA` / `END BINUTILS METADATA` regions are regenerated.
The tests independently compile the upstream MIPS, PowerPC, M68K, and SPARC
tables to compare every imported match/mask, and run the upstream S390 mnemonic
generator to check conditional instruction expansion.

## Coverage

| ISA files | Source tables |
| --- | --- |
| `arm32.isa` | A32, coprocessor, and NEON tables in `arm-dis.c` |
| `avr.isa` | `avr-dis.c`'s included `include/opcode/avr.h` table |
| `m68k.isa` | `m68k-opc.c`, including its alias table |
| `mips32.isa`, `mips64.isa` | `mips-opc.c` |
| `ppc32.isa`, `ppc64.isa` | `ppc-opc.c` |
| `s390x.isa` | `s390-opc.txt`, format/mask definitions in `s390-opc.c` |
| `sparc64.isa` | `sparc-opc.c`, including macro-expanded table entries |
| `x86.isa` | `i386-opc.tbl` |

The remaining architecture files have no corresponding table in this checkout.
In particular, this release predates AArch64 and RISC-V support. Individual
unmatched operations are listed in each enriched profile; absence from this
inventory is not evidence that an instruction is invalid.

## Field interpretation

The metadata uses the existing ISA object/array field syntax, accessible through
`qisa::Document::profile` and `qisa::Operation::fields`. It is a reference inventory
of source-table variants. A mnemonic can have multiple operand forms and CPU
requirements; consumers must apply the recorded constraints before using one.
The existing `encoding`, `operands`, `syntax`, and `semantics` fields continue to
describe the operation itself. Instruction/semantic counts in `manifest.json`
count operations, not reference variants.

- **Lookup:** ordinarily the mnemonic in `syntax`. Motorola size dots are
  normalized (`move.l` → `movel`); source aliases retain their canonical mnemonic
  and `alias_line`. x86 `pushfq`/`popfq` use GAS's `pushf`/`popf` templates with a
  size suffix. SPARC annul suffixes are represented in the source operand codes.
- **ARM:** `a32_base_bits_and_mnemonic` requires the operation's base word to
  match the source mask and resolved mnemonic template. Thumb tables are excluded.
  Disassembler `%` controls remain in `assembler`; these are not ISA placeholders.
- **Match/mask:** generally `(instruction & mask) == match`. Values are numeric,
  with C macros evaluated from the local sources. Additional operand constraints
  and table ordering can still matter.
- **M68K:** the source stores the first word in bits 31:16 and the next in bits
  15:0 (`match_layout = "first_word_high16"`). `pseudo_size_bytes` is the
  disassembler's read-size hint, not necessarily the final instruction length.
- **SPARC:** `match` is the required-one bit set; `lose` is the required-zero
  bit set; `mask` is their union. Architecture entries explicitly excluding V9
  are omitted. Flags preserve delay-slot, branch, floating-point, and alias facts.
- **MIPS:** `pinfo`, `pinfo2`, and `membership` retain the source flag expressions,
  including register effects, delay slots, and ISA membership. Assembler macros
  are marked `kind = "assembler_macro"` with a `macro_id`, and have no binary
  match/mask. MIPS16 and microMIPS are separate encodings and are not imported.
- **PowerPC:** `flags` and `deprecated` are source architecture expressions;
  `operand_codes` names the upstream operand descriptors. These remain subject
  to target CPU selection.
- **AVR:** the bit pattern and match/mask describe the first 16-bit word;
  `encoding_bits` includes a second word where specified. Constraints such as
  `r=r`, pointer restrictions, and immediate ranges remain in `operand_codes`.
- **S390:** masks and matches are big-endian instruction-width integers. Formats,
  descriptions, operand codes, minimum CPUs, and z/Architecture modes come from
  the source tables. Conditional mnemonic expansions use `s390-mkopc.c`'s tables.
- **x86:** `base_opcode`, `extension_opcode`, and `opcode_length` are assembler
  template fields, not complete instruction bytes. `D`, `W`, ModRM, REX, suffix,
  and operand-size modifiers must be applied. Operand types use upstream GAS
  order. `CpuNo64` variants are excluded.

The imported tables retain their upstream source attribution and licensing;
see the referenced files and the binutils `COPYING*` files.
