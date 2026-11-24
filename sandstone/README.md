# Sandstone

Sandstone is a retargetable assembler framework. SandAS uses built-in
[DSymspec](dsymspec/README.md) profiles by default. It also accepts
explicit SymAS databanks (`.symas`, magic `0xCEEB09`), which describe
instruction forms, operand constraints, encoding functions, syntaxes,
sections, relocations, and per-model optimization data.

The assembler also supports [ArchML `.insn`, inline Lua, source macros and a
C/C++ extension SDK](docs/assembler.md). Lua and Kaguya are built from
`third_party`; the [C11 SDK example](examples/assembler-sdk.c) combines all
three. See [validation and remaining limits](docs/assembler-linker-validation.md).

```sh
sandstone-as --target x86 --syntax intel -o out.bin prog.s
```

## Layout

| Path | Contents |
| --- | --- |
| `SYMAS.md` | Normative SymAS format reference |
| `decoder/` | SymAS databank library (`libsandstone`: reader, queries, encoder, builder) |
| `include/` | Public C/C++ headers (`sandstone.h`, `sandstone.hpp`; `domsymas.*` shims) |
| `compiler/` | Spec compiler: JSON documents → `.symas` (`sandstone-spec-compiler`) |
| `compiler/stdspec/` | Shipped `.dsymspec` architecture specifications (Lua DSL) |
| `dsymspec/` | [LLSynth compiler and standard profiles for standalone C11 assembler includes](dsymspec/README.md) |
| `assembler/` | Assembler framework (`libsandstone_assembler`) + `sandstone-as` CLI |
| `archml/` | Architecture compiler, C/C++ APIs, standalone C++ code generation and 17 profiles |
| `bindings/` | SWIG 4.0 interface (`sandstone.i`) + `generate-bindings.sh` |
| `isa/` | Small `.isa` sources with Python (`simple_isa.py`) and C++ loaders |
| `scripts/` | `spec-compiler.py` (`.dsymspec` → `.symas` driver), `compile-specs.py` |
| `tests/` | C++ smoke test, Python ISA goldens, end-to-end pipeline test |
| `docs/` | Guides (this file's siblings) + Doxygen setup |

`GUIDE.md` walks through common tasks; `INSTALL.md` covers building,
installing, and platform notes. The historical `DomSymAs` names survive
as header shims (`domsymas.h`), library alias (`domsymas`), and record
prefixes (`DOMSYMAS_*`, `DomSymAs_*`), which the format reserves for
stability.

## Quick start

Build (see `INSTALL.md` for prerequisites):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Assemble for the build's target architecture, or select a built-in
profile explicitly with `--target`:

```sh
./build/assembler/sandstone-as --target riscv64 -o hello.bin hello.s
```

Use `--bank path/to/custom.symas` to select a legacy or custom bank.

## SandSLAB code generation

The C++20 [SandSLAB library and Forth compiler example](sandslab/README.md)
use the instruction forms in `isa/` as a machine-near IR, with assembly and
binary emission and local branch fixups. Build with
`SANDSTONE_BUILD_SANDSLAB=ON` (default). DSLtk comes from MLVM's
`third_party/metatk/DSLtk`; set `MLVM_METATK_SOURCE_DIR` to use another checkout.

## ArchML architecture compilation

The [ArchML compiler](archml/README.md) validates `.am`/`.aml` architecture
profiles and generates standalone C++20 encoders/decoders, assembly templates,
and compiler metadata. It also emits JSON and canonical ArchML.
Build with `SANDSTONE_BUILD_ARCHML=ON` (default; requires Perl).

```sh
./build/archml/archml --target cpp -o riscv64.hpp archml/stdspec/riscv64.aml
```
