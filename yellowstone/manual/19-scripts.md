# Chapter 19: Linker Scripts

`qobjld -T layout.ld input.o -o program` supports `ENTRY`, `INPUT`/`GROUP`,
`SEARCH_DIR`, `MEMORY`, `REGION_ALIAS`, `SECTIONS`, assignments, `PROVIDE`, and `ASSERT`.
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
Expression parsing and AST construction are generated at build time from
`ld/ldscript.g` and `ld/ldscript.asdl` using `scripts/aurocks.pl` and
`scripts/asdl.pl`. Section/directive parsing remains recursive descent.
Arithmetic wraps modulo 2^64; literal overflow, divide-by-zero and invalid
shifts are errors. Both generated parsing and AST translation have complexity/
depth limits; malformed deeply nested input fails rather than overflowing.
Quote section/symbol names containing hyphens in expressions: unquoted `a-b`
means subtraction. Hyphens remain valid in section selectors.

## Memory and section mapping

`REGION_ALIAS("CODE", ROM)` declares an alias to an existing region or earlier
alias. It shares the underlying cursor; `ORIGIN(CODE)` and `LENGTH(CODE)` query
the same geometry. Unknown targets, duplicate names and region-range overflow
are errors. Forward alias references are not supported.

Explicit `>region` wins; an explicit address otherwise bypasses automatic
region selection. An unassigned allocated section selects the first declared
region whose attributes match. Positive `r` (read-only), `w` (writable), `x`
(executable), `a` (allocated), `i`/`l` (initialized) flags match **any** property;
flags after `!` exclude matching sections. For example, `rx` selects read-only
or executable sections; `rw!x` selects non-executable readable/writable data.
Attributes are placement selectors, not hardware protection declarations.
Regions without attributes require explicit selection. If no selector matches,
the current location-counter layout remains in effect.

`.data : ALIGN(32) { *(.data*) } >RAM` raises output alignment (power-of-two
only). `.arena (NOLOAD) : { *(COMMON) } >RAM` allocates COMMON symbols with their
native alignment and no file bytes. COMMON sizes participate in region overflow
and section overlap checks. A static script map includes region configuration,
aliases, output VMAs/sizes/file offsets and input section contributions.

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
`memory(name, origin, length[, attributes])`, `region_alias(alias, region)`,
`section(name, pattern[, address[, region]])`,
`assertion(expression, message)`. `qld.directive(name, {arguments...})` invokes
a host SDK callback. `qld.macro(name, function)` installs a Lua function.
Scripts have Lua's standard libraries and are executable build inputs.
`--check-script -T layout.lua` runs expansion and checks syntax; it does not
prove layout or relocation validity without input objects.

The section builder also accepts a table:

```lua
qld.section('.data', {
  patterns={'.data', '.data.*'}, region='RAM', alignment=32,
  keep=true, sort='name'
})
qld.section('.bss', {patterns={'.bss', 'COMMON'}, region='RAM', noload=true})
```

Supported keys are `patterns` (required ordered list), `address`, `region`,
`alignment`, `noload`, `keep`, and `sort` (`name` or `alignment`). Unknown keys
and invalid selector/name strings fail. Expression strings are still ordinary
linker expressions, not sandboxed inputs. See the executed `ld/examples/memory.*`.

## Current limits

This is a supported subset, not GNU ld parity. `PHDRS`, `VERSION`, `INSERT`,
`AT`/LMA, hidden script symbols, `OUTPUT`, `OUTPUT_ARCH`, `OUTPUT_FORMAT`,
`EXCLUDE_FILE`, `INPUT_SECTION_FLAGS`, and initialization-priority sorting are
rejected. Use `-o` for output; ELF input geometry chooses the architecture.
File-selective COMMON allocation and discarding COMMON symbols are unsupported;
use `*(COMMON)` in a normal output section.
Full dynamic-loader metadata, GOT/PLT/TLS linking, and general shared-library
resolution remain outside this static-script implementation.

The executable regression `ld/tests/scripts.py` compiles native ELF inputs,
links/runs them, checks emitted data, and checks negative layout/expression cases.
