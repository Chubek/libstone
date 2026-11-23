# Cephyr AMD64 GAS coverage

Use `sandstone-as --bank amd64.symas --syntax att -f elf -o out.o input.s`.
Rebuild `amd64.symas` from the current `compiler/stdspec/amd64.dsymspec`.
The spec generates per-form encoder programs for the `att` syntax: opcodes,
mandatory prefixes, operand widths, ModR/M direction and extension fields,
immediate widths, relocations and REX.W come from the bank. The source
parser binds register numbers from the target and normalizes addressing;
the same bounded SymAS interpreter handles layout and final bytes. No host
assembler library or compiler subprocess is used by SandAS.

## Source inventory

| Producer and purpose | Emitted instruction or directive families |
| --- | --- |
| `cornerstone/Archon/archon/irgen/native/AMD64GAS.c`: frame, parameters, constants, memory | `subq`, `addq`, `leaq`, `movabsq`, `movb/w/l/q`, `movsbq`, `movzbq`, `movswq`, `movzwq`, `movslq`, `movzbl`, `movzwl`; `movss/sd`, XMM↔GPR `movd/q`, vararg `movaps` stores |
| Same file: integer arithmetic/control | `imulq`, `divq`, `idivq`, `cqto`, `andb/l/q`, `orw/q`, `xorl/q`, `cmpq`, `testb/q`, `shlq`, `shrq`, `sarq` (`$imm8`/`%cl`); `sete/l/b/be/np`; direct and register-indirect `call`, `ret`, `jmp`, `jz`, `jnz`, `jne`, `jb`, `jns`, `jno`, `int3` |
| Same file: scalar floating point | `add/sub/mul/div` with `ss`/`sd`, `comiss/sd`, `ucomiss/sd`, `cvtsi2ssq/sdq`, `cvttss2siq`, `cvttsd2siq`, `cvtss2sd`, `cvtsd2ss`, `btcq`, `btsq` |
| Same file: x87 long double | `fldt/s/l`, `fstpt/s/l`, `fildq`, `fistpq`, `faddl`, `faddp`, `fmulp`, AT&T `fsubp/rp`, `fdivrp`, `fucomi`, `fucomip`, `fcomip`, `fstp`, `fxch`, `fnstcw`, `fldcw` |
| `ScalarGAS.c`: shared function/global emission, wide stack slots | `.text/.data/.bss`, `.globl`, `.p2align`, `.type`, `.size`, `.quad`, `.zero`, `.note.GNU-stack`; `movw`, `movzwq`, `fldt`, `fstpt` |
| `cornerstone/Cephyr/src/lower_globals.cpp`, `main.cpp`, `sandslab_opt.cpp`: object images, call adapters and constant shortening | `.local/.globl`, `.balign`, `.byte`, `.quad symbol±constant`, `.data.rel.ro`, `.set alias,symbol`; host trampoline `.hidden`, `movabs`, `jmp *%r11`; shortened constants as `.byte` |

The supported memory grammar is `[disp](%base[,%index[,scale]])`, including
index-only and `%rip` forms. Scales are 1, 2, 4 or 8; displacements are
signed 32-bit. `%rip` references carry `R_X86_64_PC32` or non-relaxing
`R_X86_64_GOTPCREL`. Direct calls support `@PLT`. The ELF addend accounts
for an immediate following the displacement (`movq $7,label(%rip)` needs
-8, not -4). Local jumps use long encodings. `movabsq` accepts a full
64-bit hex pattern; `movq $imm,dest` requires a sign-extendible imm32.

This inventory is for **provider-emitted forms**. It does not promise all
combinations for an x86 mnemonic. High-byte registers (`%ah` etc.), segment
overrides, AVX/AVX-512 syntax, numeric local labels, assembler relaxation and
arbitrary GAS expressions are outside this subset. Chained or cyclic symbolic
`.set` aliases are rejected. The current AMD64 Cephyr frontend also rejects
the RV64-specific by-value aggregate ABI fixture, before assembly.

## Validation

`sandstone_spec_pipeline` checks golden bytes and relocation tuples for
extended registers, SIB, disp8/disp32, RIP-relative and GOT/PLT references,
x87 operand direction, and invalid widths/registers/addresses/immediates.

On Linux x86-64, the following also compiles **real C sources** through Cephyr
and SandAS, runs the scalar ABI and floating fixtures against independent C
harnesses, runs the x87 suite with and without PIC/QML, links a three-TU
static program with qobjld, and loads and calls a qobjld-linked PIC DSO:

```sh
python3 sandstone/tests/amd64_cephyr.py --cephyr /path/to/cephyr \
  --assembler /path/to/sandstone-as --bank /path/to/amd64.symas \
  --qobjld /path/to/qobjld
```
