# Chapter 19: Linker Scripts

`qobjld -T layout.ld input.o -o program` supports `ENTRY`, `INPUT`/`GROUP`,
`SEARCH_DIR`, `MEMORY`, `SECTIONS`, assignments, `PROVIDE`, and `ASSERT`.
An explicit `-e` takes precedence over `ENTRY`. Scripts can supply all inputs;
`SEARCH_DIR` is applied before resolving `-l` libraries. Entry symbols can
select archive members. Use ELF input objects, for example from `cc -c`.

```ld
ENTRY(_start)
MEMORY { RAM : ORIGIN = 0x401000, LENGTH = 64K }
SECTIONS {
  .text : { *(.text) *(.text.*) } >RAM
  .data : { BYTE(0xaa) . = ALIGN(16); data_begin = .; *(.data) } >RAM
  .meta : { SHORT(0x1234) LONG(42) QUAD(7) FILL(0xcc) . += 3; } >RAM
  .bss (NOLOAD) : { *(.bss) } >RAM
}
ASSERT(SIZEOF(.meta) == 17, "unexpected metadata size")
```

Section-body statements execute in source order. Data uses target endianness;
alignment and padding affect symbol addresses and relocation offsets. Fill
values currently require a single byte. Backward section location counters,
overlapping sections, misalignment, MEMORY overflow, and invalid data values
are diagnosed. Script file images are limited to 256 MiB.

Expressions support unsigned 64-bit arithmetic, bitwise/logical/comparison
operators, `?:`, K/M/G suffixes, `ALIGN`, `ALIGNOF`, `ADDR`, `SIZEOF`,
`LOADADDR` (equal to VMA), `DEFINED`, `ORIGIN`, `LENGTH`, `ABSOLUTE`, `MAX`,
`MIN`, `CONSTANT`, and `SEGMENT_START`. Evaluation is source-ordered; forward
symbol/section references requiring lazy evaluation are unsupported.

`INCLUDE "relative.ld"` occupies its own line and resolves from the containing
script. Cycles are rejected. Extensions occupy separate `@name args` lines;
see [the SDK](26-extension.md). Expansion allows 64 levels, 100,000 steps and
16 MiB of generated source.

## Lua scripts

A `.lua` filename or a first line `--!lua` selects bundled Lua/Kaguya. Lua runs
once before discovery/layout and emits ordinary linker commands:

```lua
qld.entry('_start')
qld.macro('ordinary', function(name) qld.section(name, name .. '*') end)
for _, name in ipairs({'.text', '.data', '.bss'}) do qld.ordinary(name) end
qld.emit('SECTIONS { .meta : { LONG(42) } }')
```

Builders: `qld.emit(text)`, `entry(symbol)`, `input(path)`, `search_dir(path)`,
`define(name, expression)`, `provide(name, expression)`,
`memory(name, origin, length)`, `section(name, pattern[, address[, region]])`,
`assertion(expression, message)`. `qld.directive(name, {arguments...})` invokes
a host SDK callback. `qld.macro(name, function)` installs a Lua function.
Scripts have Lua's standard libraries and are executable build inputs.
`--check-script -T layout.lua` runs expansion and checks syntax; it does not
prove layout or relocation validity without input objects.

## Current limits

This is a supported subset, not GNU ld parity. `PHDRS`, `VERSION`, `INSERT`,
`AT`/LMA, hidden script symbols, `OUTPUT`, `OUTPUT_ARCH`, `OUTPUT_FORMAT`,
`EXCLUDE_FILE`, `INPUT_SECTION_FLAGS`, and initialization-priority sorting are
rejected. Use `-o` for output; ELF input geometry chooses the architecture.
MEMORY attributes are descriptive; explicit `>region` selects placement.
Full dynamic-loader metadata, GOT/PLT/TLS linking, and general shared-library
resolution remain outside this static-script implementation.

The executable regression `ld/tests/scripts.py` compiles native ELF inputs,
links/runs them, checks emitted data, and checks negative layout/expression cases.
