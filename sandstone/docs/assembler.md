# Assembler framework

The assembler (`assembler/`, library `sandstone_assembler`, CLI
`sandstone-as`) defaults to built-in DSymspec profiles. The default
profile matches the build's target architecture when available; use
`--target` to select one explicitly. The built-in backend needs neither a
`.symas` file nor `sandstone-spec-compiler` at runtime.

```sh
sandstone-as --target amd64 --syntax intel -o out.bin prog.s
sandstone-as --target riscv64 -f obj -o prog.obj.json prog.s
sandstone-as --target rv64im -f elf -o prog.o prog.s
sandstone-as --bank custom.symas -o out.bin prog.s
```

`--bank` is an explicit choice of the legacy backend, not an automatic
retry on assembly errors. Existing C/C++ calls that pass a `.symas` path
or image continue to use that bank as supplied. The C constructor
`sandstone_asm_new` can take a null path to select the default profile.
Both backends share source processing, the SDK, and output writers.
Built-in instruction coverage depends on the profile and on SandAS's
operand syntax support; not every generated recipe is usable from source.

## Native Windows and Darwin objects

`-f coff` writes a relocatable Windows COFF `.obj` (default `out.obj`).
`-f macho` writes a 64-bit Darwin `MH_OBJECT` (default `out.o`, macOS 11
build-version command). Both writers run inside SandAS with no LLVM or
platform toolchain dependency. Use `obj` for the existing JSON format.

Select a bank with the matching target **before** assembly. With
`sandstone-spec-compiler` on PATH, for example:

```sh
python3 sandstone/scripts/spec-compiler.py --object-profile windows --validate \
  -o amd64-win.symas sandstone/compiler/stdspec/amd64.dsymspec
sandstone-as --bank amd64-win.symas -s att -f coff -o routine.obj routine.s
python3 sandstone/scripts/spec-compiler.py --object-profile darwin --validate \
  -o arm64-mac.symas sandstone/compiler/stdspec/aarch64.dsymspec
sandstone-as --bank arm64-mac.symas -f macho -o routine.o routine.s
```

Both profiles support little-endian, 64-bit `amd64` and `aarch64`. They
reuse the standard encoder and select target metadata `COFF`/`Win64` or
`Mach-O`/`Darwin`, with calling convention `<profile>-<arch>`. The
assembler does not translate calling conventions, decorate symbol names,
or accept arbitrary MASM/Apple assembler syntax. Supply code for the
destination ABI and spell names as the linker expects (e.g. `_main` on macOS).

| Input / fixup | COFF | Mach-O |
| --- | --- | --- |
| `.text`, `.rodata`, `.data`, `.bss` | `.text`, `.rdata`, `.data`, `.bss` | `__TEXT,__text`, `__TEXT,__const`, `__DATA,__data`, `__DATA,__bss` |
| Symbolic data with signed addend | `.long`, `.quad` | `.quad` |
| AMD64 call/jump, RIP-relative address | REL32 | BRANCH, SIGNED |
| AArch64 B/BL, ADRP, ADD low 12 | BRANCH26, PAGEBASE_REL21, PAGEOFFSET_12A | BRANCH26, PAGE21, PAGEOFF12 |

BSS takes virtual space only. Section alignment is a power of two (COFF
up to 8192 bytes, Mach-O up to 16 MiB). `.global`/`.local` bindings,
section-defined and undefined symbols are preserved. Both formats use
in-place data addends; AArch64 instruction fixups require zero addends.
Custom sections, TLS/GOT fixups, nonnumeric `.set` aliases, `.hidden`,
and unwind/COMDAT generation are outside this writer's scope.

The C SDK provides `sandstone_asm_assemble_object(ctx, source, source_name,
output_path, format)` (`elf`, `coff`, `macho`); C++ provides
`Assembler::assemble_object(source, source_name, output_path, format)`.
These one-shot calls select section-relative layout, use the existing
context configuration and return the file as their result.

`tests/native_objects.py --build-dir BUILD` inspects all four object
profiles with `llvm-readobj` and `llvm-nm`. Add `--lld` to cross-link all
four and verify the linked AArch64 branch, page address and pointer
addends. `--qobjld /absolute/path/to/qobjld` exercises Yellowstone's native
AMD64 static PE and Mach-O linkers and checks the freestanding result on
Linux x86-64. AArch64 static PE and Mach-O linkage is not currently
supported by native qobjld.

## Native ELF objects and Cephyr

`-f elf` (or `--format elf`) selects little-endian ELF64 `ET_REL` before
layout. The CLI defaults to `out.o` in this mode. The C SDK entry point is
`sandstone_asm_assemble_elf(ctx, source, source_name, output_path)`;
it uses the context's syntax, features and source hooks and returns 0 on
success. The file is the result; this call clears intermediate layout data.
The encoder and ELF writer run inside SandAS. `-f bin` (default) still
writes a flat image and `-f obj` still writes `sandstone-obj-1` JSON.

Build the single-target `.symas` bank once, e.g. from the repository root
with `sandstone-spec-compiler` on PATH:

```sh
python3 sandstone/scripts/spec-compiler.py --validate -o rv64im.symas \
  sandstone/compiler/stdspec/rv64im.dsymspec
sandstone-as --bank rv64im.symas --syntax default -f elf -o routine.o routine.s
llvm-readelf -hSWsr routine.o
```

| Cephyr target | Standard spec | `--syntax` | ABI |
| --- | --- | --- | --- |
| `x86_64-linux-gnu` | `amd64.dsymspec` | `att` (`intel` for Intel-order sources) | SysV AMD64 |
| `aarch64-linux-gnu` | `aarch64.dsymspec` | `default` | AAPCS64 |
| `riscv64-linux-gnu` | `rv64im.dsymspec` | `default` | LP64, no RVC |

`riscv64.dsymspec` defaults to LP64D with RVC enabled; changing the filename
or selecting a different syntax does not change the ABI. Rebuild old RISC-V
banks: a four-byte `jal` must use `R_RISCV_JAL`, not the two-instruction
`R_RISCV_CALL`.

Use SandAS as a raw assembler for a bank-compatible `.s` input from Cephyr:

```toml
target = "x86_64-linux-gnu"
assembler_style = "as"
[tools]
assembler = ["/path/to/sandstone-as", "--bank", "/path/to/amd64.symas", "--syntax", "att", "-f", "elf"]
```

```sh
cephyr --config sandas.toml -c -x assembler routine.s -o routine.o
```

The writer preserves section-relative symbols, `SHT_NOBITS` sizes,
power-of-two alignment and explicit-addend `SHT_RELA` entries (including
references to *defined* labels). Section attributes default to bank metadata;
`.section NAME,"aw",@nobits` and the `a`, `w`, `x` flag subset are supported.
Use `.balign` for bytes or `.p2align` for a power-of-two exponent.
Symbols default to local when defined, global when undefined. `.global`,
`.local`, numeric `.equ`, `.type NAME,@function`/`@object` and
`.size NAME,.-NAME` or a numeric size are supported. Unknown relocation
kinds and unsupported relocatable expressions are errors.

For AMD64, `cephyr --config sandas.toml -c routine.c -o routine.o` also
handles the bundled scalar GAS provider's instruction forms. See the
[AMD64 source inventory, validation and limits](amd64-cephyr.md).

## Pipeline

0. **Expand source** (`preprocess.cpp`): the optional `--lua` hook runs first.
   Includes, conditional assembly, macros, repetitions, SDK directives and
   inline Lua then execute in source order. Generated output re-enters this
   processor. Expansion is limited to 64 nested calls, 100,000 steps and
   16 MiB of source.
1. **Optionally rewrite expanded assembly** (`asm_opt.cpp`): `--asm-opt`
   supplies one complete replacement for the expanded assembly.
2. **Parse** (`parser.c`): each line is `[label:] (directive | mnemonic
   operands...)`. Comments follow the selected syntax record (a comment
   token starts a comment at line start, or when surrounded by
   whitespace, so `#4`-style immediates survive a `#` comment).
   Operands split on the syntax separator (whitespace when the separator
   is blank), aware of `[...]` memory expressions and quotes. AT&T-style
   syntaxes (`operand_order == 1`) have their operands un-reversed before
   encoding.
3. **Operand classification**: registers resolve through the bank
   (syntax prefix stripped, case-insensitive fallback), `$`-prefixed and
   bare literals become immediates, `[base+...]` becomes a memory
   operand, anything else a symbol reference.
4. **Binarize** (`binarizer.c`): pass 1 assigns addresses and sizes,
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
`.p2align`, `.org`), and symbol metadata (`.type`, `.size` in ELF mode).
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
Lua ABI is guessed. Each assembly invocation starts a fresh preprocessing
Lua state.
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
`preprocess(source, name)` function returning a string, before expansion.
Preprocessing Lua runs with its standard libraries; these are executable build
scripts.

## Optional post-expansion Lua hook

```sh
sandstone-as --bank x86.symas --syntax intel --asm-opt optimizer.lua \
    -o prog.bin prog.s
```

`optimizer.lua` defines one global function that returns the **complete**
replacement assembly (return `source` unchanged to keep it as-is):

```lua
function asm_optimize(source, name)
    return source
end
```

Sandstone calls it once per assembly unit, after all source expansion and
before parsing, layout and relocation resolution. `name` is the same
top-level source name passed to `preprocess` and used for parsing diagnostics.
For multiple CLI inputs they are still concatenated into one unit, with the
first input filename as `name`. The string assembly API uses `src_name`, or
`<string>` if it is null. The returned string is parsed directly:
macros, includes, SDK directives and inline Lua are **not** expanded again.
Sandstone does not rewrite instructions on its own. Use a script written for
the selected bank and syntax.

The hook can read these fields in its own read-only `asm_context` table:

- `arch`: the architecture name from the loaded SymAS bank, or `nil` if no
  bank provides an unambiguous architecture.
- `syntax`: the selected SymAS syntax record's name, or `nil` if none is
  selected.
- `riscv64_nop_alias`: `true` only for an RV64 bank when `addi x0,x0,0` and
  `nop` each produce the same four bytes and neither produces a relocation.
  The check uses the configured syntax and enabled features. Otherwise the
  field is `false`.

The hook runs in its own fresh Lua state, with no `lsandas`, SDK callbacks or
preprocessing globals. It has Lua's base, string, table, math and UTF-8
libraries, but no file, module, console or dynamic code loading APIs,
`setmetatable`, `rawset` or `xpcall`. Script and output text are each limited
to 16 MiB and cannot contain NUL bytes. Lua memory is limited to 128 MiB and
execution to 50 million Lua VM instructions. These limits are not a sandbox
or a wall-clock time limit; use trusted build scripts.

The C API can install a copy with `sandstone_asm_set_asm_opt_hook(ctx, text,
size)` or `sandstone_asm_load_asm_opt_hook(ctx, path)`, and disable it with
`sandstone_asm_clear_asm_opt_hook(ctx)`. Installation checks syntax without
running the script; a failed installation leaves the previous hook in place.
If the function is missing, fails or returns invalid output, the assembly
fails instead of falling back to the original text. Failed assembly with
the hook enabled exposes no partial C assembly results, and the CLI leaves
any existing output file untouched. `sandstone_asm_preprocess` never calls
`asm_optimize`.

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

Yellowstone consumes the native ELF objects described above. There is no
direct JSON-object bridge. `.type` and `.size` remain no-ops in flat/JSON mode.
