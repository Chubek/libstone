# Assembler framework

The assembler (`assembler/`, library `sandstone_assembler`, CLI
`sandstone-as`) assembles any dialect a `.symas` bank describes. Nothing
about an instruction set is hard-coded: form resolution, operand checking,
and byte emission all go through the bank.

```sh
sandstone-as --bank x86.symas --syntax intel -o out.bin prog.s
sandstone-as --bank riscv64.symas -f obj -o prog.obj.json prog.s
```

## Pipeline

0. **Expand source** (`preprocess.cpp`): includes, conditional assembly,
   macros, repetitions, SDK directives and inline Lua execute in source
   order, before layout. Generated output re-enters this processor. Expansion
   is limited to 64 nested calls, 100,000 steps and 16 MiB of source.
1. **Parse** (`parser.c`): each line is `[label:] (directive | mnemonic
   operands...)`. Comments follow the selected syntax record (a comment
   token starts a comment at line start, or when surrounded by
   whitespace, so `#4`-style immediates survive a `#` comment).
   Operands split on the syntax separator (whitespace when the separator
   is blank), aware of `[...]` memory expressions and quotes. AT&T-style
   syntaxes (`operand_order == 1`) have their operands un-reversed before
   encoding.
2. **Operand classification**: registers resolve through the bank
   (syntax prefix stripped, case-insensitive fallback), `$`-prefixed and
   bare literals become immediates, `[base+...]` becomes a memory
   operand, anything else a symbol reference.
3. **Binarize** (`binarizer.c`): pass 1 assigns addresses and sizes,
   iterating up to 8 rounds until the layout converges, so `.equ`
   constants and directive sizes may reference forward labels; pass 2
   re-encodes and resolves relocations against locally defined labels.
   Self-referential layouts (an address depending on itself) fail with a
   `layout failed to converge` diagnostic instead of silently emitting
   arbitrary bytes. Symbols in numeric, relocation-less operand positions
   must be defined; other undefined symbols become relocations.

## Operand-name binding

SymAS operands are positional; encoding functions use names (`dst`,
`src`, `rd`, `rs1`, `imm`, `sym`, `cond`, ...). The assembler binds each
positional operand under every conventional name for its position
(operand 0 also as `dst`/`rd`, operand 1 also as `src`/`rs1`, operand 2
also as `rs2`), plus `imm` for immediates, `sym`/`target` for symbols,
`mem` for memory operands, and `cond` for conditions. Unused bindings
are ignored by the encoder. `COND` operands accept x86 condition names
(`e`, `ne`, `b`, `ae`, ...) as well as numbers.

## Relocation handling

- Absolute relocs against local labels patch as `S + A`.
- PC-relative relocs patch as `S + A - P`, where `P` is the end of the
  containing instruction when the field is a strict suffix (trailing
  `rel32`-style immediates) and the start of the instruction otherwise
  (whole-word fields).
- The field location always comes from the encoder-reported occurrence
  offset; whole-instruction fields recorded at the instruction end are
  normalized to the trailing `width` bits.
- Fields sharing bytes with opcode bits (nonzero placeholder) are never
  patched: they need relocation-type-specific bit splicing (e.g. RISC-V
  `jal`/`branch` immediates) that only a linker performs. Such
  relocations — and relocations against undefined symbols — are kept for
  `obj` output and rejected for flat `bin` output with a diagnostic.

## Directives (`directives.c`)

Sections (`.text` `.data` `.rodata` `.bss`, `.section NAME`), symbols
(`.globl`/`.global`, `.extern`, `.equ`/`.set` with fixpoint evaluation),
data (`.byte` `.short`/`.hword`/`.2byte` `.word`/`.long`/`.4byte`
`.quad`/`.8byte`, `.uleb128` `.sleb128`, `.ascii` `.asciz`
`.string`, `.zero`/`.space`/`.skip`, `.fill`, `.align`/`.balign`
`.p2align`, `.org`), and GAS-compatible no-ops (`.type`, `.size`).
`.align N` follows GAS (`2^N` for small `N`; `.balign` takes bytes).
Integer widths are 1, 2, 4 and 8 bytes respectively; `.word` is always
4 bytes. `.asciz` terminates **each** string. Hex/octal escapes are checked.
Expressions use signed 64-bit integers, symbols, parentheses, arithmetic,
shifts, bitwise, comparison and logical operators with C precedence. Invalid
shifts, division by zero and signed arithmetic overflow are errors.

## Source directives and macros

- `.macro NAME parameters` ... `.endmacro` (alias `.endm`): comma-separated
  arguments, defaults (`n = 7`), final variadic arguments (`values...`),
  `\name`/`\1` substitution, unique `\@`, and `local label` hygiene.
- `.if`, `.elseif`, `.else`, `.endif`, `.ifdef`, `.ifndef`; inactive branches
  neither execute Lua nor open includes. Conditions require known constants.
- Nested `.rept COUNT` ... `.endr`; `.purge NAME` removes a source macro.
- `.include "file"` and `.incbin "file"[,OFFSET[,COUNT]]` resolve relative to
  the containing file. Include cycles and invalid binary ranges are errors.
- `.struct NAME`, `.field NAME,SIZE`, `.align BYTES`, `.endstruct` define
  `NAME.field` and `NAME.size` constants without emitting bytes.
- `.enum NAME` ... `.endenum` accepts `ITEM = VALUE` or incrementing `ITEM`;
  both `ITEM` and `NAME.ITEM` become constants.
- `.assert EXPR[,MESSAGE]`, `.error MESSAGE`, `.pushsection`, `.popsection`,
  `.previous`.

## Bundled Lua and Kaguya (`lua.cpp`)

The assembler links `third_party/lua` through `third_party/kaguya`; no system
Lua ABI is guessed. Each assembly invocation starts a fresh Lua state.
Blocks begin with `<:` at the start of a source line and end with `:>` outside
Lua strings/comments. Globals persist between blocks in that invocation;
block-local variables follow ordinary Lua scope. Known constants are available
as globals (use `_G['Packet.size']` for names containing dots).

```asm
.equ N, 3
<:
lsandas.injectf('.byte ~li', {1, 2, N})
lsandas.register_directive('.twice', function(arg)
  lsandas.injectf('.byte %d, %d', tonumber(arg), tonumber(arg))
end)
:>
.twice 4
```

`inject` inserts strings/numbers verbatim, or joins a sequence with commas.
`injectf` supports Lua `string.format` plus `~li`, `~lx`, `~s`. A sequence
can supply repeated groups of format arguments; formatted groups end in a
newline. `lsandas.symas` exposes targets/opcodes/forms, registers, features,
sections, relocations, syntaxes and linkages.

`--lua hook.lua` / `sandstone_asm_load_hook` also supports a whole-source
`preprocess(source, name)` function returning a string. Lua runs with its
standard libraries; these are executable build scripts.

## ArchML

```sh
sandstone-as --archml archml/stdspec/amd64.aml -o out.bin program.s
```

Use `.insn nop` or `.insn NAME, CONSTANT...`, with operand order and range
checked by the profile encoder. `--bank` and `--archml` can be combined:
ordinary mnemonics use SymAS; `.insn` uses ArchML. Symbolic ArchML relocation
operands are unsupported. With `SANDSTONE_BUILD_ARCHML=OFF`, the public loader
returns a diagnostic. ArchML parsing works independently of the working directory.

## APIs

- C: `assembler/assembler.h` (`sandstone_asm_*`; borrowed getters, owned
  `char *` from `sandstone_asm_preprocess`).
- C++: `assembler/assembler.hpp` (`sandstone::Assembler`, RAII,
  `std::vector` views, exceptions).
- Extension SDK v1: `assembler/sdk.h`, `sandstone_asm_register_directive`,
  `sandstone_asm_unregister_directive`, `sandstone_asm_define_macro`,
  `sandstone_asm_load_archml`, `sandstone_asm_new_archml`.

Callbacks receive argument text and a source sink. Return zero on success;
write diagnostics into the supplied buffer on failure. Sink failures are
sticky, and built-in/duplicate registrations are rejected. Registrations
survive assembly calls; callback user data is borrowed until unregister or
context destruction. Do not recursively assemble on the same context.
See the executable [C SDK example](../examples/assembler-sdk.c) and
[installed build instructions](../INSTALL.md).

Output is flat binary or Sandstone JSON objects, not ELF. Yellowstone currently
consumes ELF inputs; there is no direct JSON-object bridge. This is a defined
assembler subset, not complete GAS/NASM compatibility; `.type`/`.size` remain
accepted metadata no-ops.
