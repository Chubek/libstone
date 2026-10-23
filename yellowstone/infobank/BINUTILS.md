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

## Complete architecture inventory

The bundle covers **all 70 architecture cases** in this checkout's
`opcodes/disassemble.c`. `manifest.json` records the exhaustive mapping in
`binutils_coverage`, including the dispatcher source digest. Existing ARM, AVR,
M68K, MIPS, PowerPC, S390, and SPARC descriptions supply seven architecture
families; **63 generated files** supply the other dispatcher targets. `i386.isa`
is the 32-bit inventory, alongside the existing AMD64 `x86.isa`.

```sh
python3 yellowstone/scripts/generate_binutils_isa.py
python3 yellowstone/scripts/generate_binutils_isa.py --check
python3 -m unittest discover -s yellowstone/scripts -p 'test_*isa*.py'
```

The generator also updates manifest operation counts and architecture coverage.
It refuses to replace a file that does not carry its generated-file marker.
Adding an upstream dispatcher target without an importer mapping is an error.

### Generated description conventions

- Each operation groups the variants of one **source mnemonic/template**.
  Sanitized ISA identifiers retain the original spelling in `mnemonic`.
- `source_fields` preserves the upstream initializer's ordered C expressions,
  including operand descriptors, flags, encoding patterns, and macro expansions.
  Where supplied, `field_names` labels those columns. These are source records,
  not a uniform executable encoding schema.
- CGEN descriptions join the descriptor and opcode tables by their shared index,
  retaining instruction IDs, bit widths, syntax fields, attributes, and format
  match/mask values. `match_bits` is the CGEN format's mask width, which can be
  smaller than the full instruction.
- Blackfin uses decoder families with dispatch predicates and display fragments;
  RX uses its declarative decoder patterns. Z80 includes callback-generated
  arithmetic, bit, and block-operation names.
- Xtensa obtains its configured opcode table from `bfd/xtensa-modules.c`, the
  ISA module used by `opcodes/xtensa-dis.c`. SPU and C6X include their upstream
  instruction headers. SH includes both SHcompact and SHmedia tables.
- L1OM and K1OM use the shared x86 disassembler's 64-bit template inventory;
  the backend identity is explicit. RS/6000 is filtered to the
  `PPC_OPCODE_POWER` dialect selected by `print_insn_rs6000`.
- Word/address sizes and `bits_per_byte` are taken from the default BFD CPU
  descriptor, retained alongside its source reference. These include non-octet
  addressable units on TI DSPs. Byte order is supplied by the object file.
- Execution semantics are explicitly **`(unknown)`**. Register typing,
  instruction classes, scheduling, allocation, and optimization legality require
  further architecture-specific modeling. `misc` is the unclassified operation
  class, and the compiler sections record the missing models explicitly.
- `encoding` blocks are marked `upstream_reference`; numeric matching information
  stays with its source variant. The generated files support inventory/query
  consumers without presenting incomplete operand definitions as usable machine
  encoders. Manifest counts include mnemonic/template groups and decoder families,
  rather than claiming a hardware instruction census.

The generated inventories are checked against independently compiled upstream
macro tables, the dispatcher, and their manifest counts.

## Enrichment of existing descriptions

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

Architecture files outside these importers, such as AArch64 and RISC-V, have no
corresponding table in this older checkout. Individual
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
