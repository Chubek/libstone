# BFDWeave

A Lua 5.4 DSL generating allocation-free, target-specific ELF readers in C99.
This is **not libbfd**, a compatible ABI, or a dynamic loader. See
[BFDCollection](../../BFDCollection/README.md) for the supported targets.

Add this directory to `package.path`; both `require("bfdweave")` and
`require("BFDWeave")` work.

```lua
return require("bfdweave").elf {
  name="Example", target="elf64-x86-64", arch="i386",
  class=64, endian="little", machine=62,
  source=".info/binutils/bfd/elf64-x86-64.c; include/elf/common.h",
}
```

`elf(table)` snapshots and validates the description. `load(path)` loads a Lua
description file. `description:emit_header()` and `:emit()` generate `NAME.h`
and a C translation unit including it. All attributes are required; unknown
ones fail. `machine` is a 1..65535 ELF EM number, **not a BFD machine number**.
Class is 32/64; endian is `big`/`little`; namespace begins uppercase. Numeric
attributes must be Lua integers. Source text is safely escaped into metadata.

## Generated contract

* `NAME_info()` returns immutable target identity/provenance.
* `NAME_open(data, size, &view)` borrows the complete immutable byte buffer.
  It verifies magic, class, byte order, machine, ELF versions, type 1..4,
  standard header/entry sizes, tables, payload spans and power-of-two alignments.
  It supports ELF extended section/program counts and string-table indices.
  PT_LOAD file size must not exceed memory size. This is structural validation,
  **not exhaustive ELF/ABI verification**.
* `NAME_read_section(&view, index, &section)` and `NAME_read_segment(...)`
  return normalized unsigned 64-bit addresses/sizes, never host-layout casts.
* `NAME_section_data(&view, index, &pointer, &size)` returns a borrowed byte
  span. SHT_NULL and SHT_NOBITS return NULL/0; the section's logical size is
  still available through `read_section`.
* `NAME_section_name(&view, index, &pointer, &length)` verifies the name offset
  and NUL termination within `.shstrtab`. String validation happens on access,
  so a structurally valid file can still have an invalid section name.

Status: 0 success, -1 target mismatch, -2 truncated/out-of-buffer file data,
-3 malformed/invalid argument/index, -4 missing section-name table. Outputs
are unchanged on error. Input/output objects must not overlap. Keep backing
bytes alive and unchanged for the lifetime of every view/pointer; do not forge
or modify views. There is no hidden mutable state, allocation or file I/O.

No symbols, relocations, machine-flag interpretation, OSABI filtering,
compressed-section decoding, executable mapping, writing, archives, PE/COFF,
Mach-O or linking are implemented. Target names record binutils provenance,
not identical backend recognition behavior. Bounds checks use subtraction and
division before pointer arithmetic, avoiding offset/count multiplication overflow.
