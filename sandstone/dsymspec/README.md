# Standalone DSymspec includes

This build path compiles `stdspec/*.dsymspec` into architecture-profile `.i`
files. SandAS embeds these profiles for its default backend, and C11 or
C++ consumers can use the same headers directly. The legacy
`compiler/stdspec` → `.symas` pipeline is unchanged; SandAS selects it
when passed a `.symas` bank.

## Build

From the repository root, using CMake, Perl and Python:

```sh
cmake -S sandstone -B build/dsymspec \
  -DSANDSTONE_BUILD_DSYMSPEC=ON \
  -DSANDSTONE_BUILD_COMPILER=OFF -DSANDSTONE_BUILD_ASSEMBLER=OFF \
  -DSANDSTONE_BUILD_ARCHML=OFF -DSANDSTONE_BUILD_SANDSLAB=OFF
cmake --build build/dsymspec --target sandstone-dsymspec
```

The headers are generated under `build/dsymspec/dsymspec/generated/`. A
profile's file name is the source stem, e.g. `riscv64.i` and `rv64im.i`;
the latter describes the RV64IM profile of RISC-V, not a separate ISA.
In a CMake consumer, link the header-only `Sandstone::DSymspec` target to
add the appropriate include paths. Alternatively, add the generated
directory and `sandstone/dsymspec/` to your compiler's include paths.
An installation puts `DSymspec.h` and the `.i` files together under
`include/sandstone/dsymspec/`.

To check the generated includes in strict C11 and C++11:

```sh
cmake -S sandstone -B build/dsymspec -DSANDSTONE_BUILD_TESTS=ON
cmake --build build/dsymspec --target sandstone_dsymspec_c sandstone_dsymspec_cpp
ctest --test-dir build/dsymspec -R sandstone_dsymspec --output-on-failure
```

If `llvm-mc` is installed, CTest also checks the byte fixtures against LLVM.
The fixture check skips unregistered targets and is not needed for generation.
With LLVM 22 or newer and the corresponding object tools installed, CTest
also runs the expanded AMD64, RISC-V and AArch64 differential checks
(Python 3.10 or newer for AMD64 and RISC-V).

## Compile a custom profile

Use the same LLSynth-generated frontend with a different `.dsymspec` file.
From the repository root:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T python3 \
  --driver-mode=library -o dsymspec_frontend.py sandstone/dsymspec/DSymspec.llsynth
python3 sandstone/dsymspec/dsymspec.py --frontend dsymspec_frontend.py \
  -o myprofile.i myprofile.dsymspec
```

The input uses the declarative, Lua-like syntax in `stdspec/`. LLSynth
parses it and a bounded evaluator expands declarations; it does not run a
Lua interpreter, load Lua modules, or allow host calls. The `require("backend")`
collector is built in. The shipped `riscv32` and `rv64im` entries can import
their adjacent `riscv64.dsymspec`; general-purpose `loadfile` is not supported.
Expansion has independent limits on input size, computation, strings,
declaration count and generated header size.
For a custom profile, the name passed to `require("backend").new(...)`
determines the generated C identifiers, regardless of the output path.
If you omit `-o`, the compiler writes `<profile>.i` in the current directory.

## Use in an assembler

Include the profile and use its `dsymspec_<profile>` descriptor. For example,
once an assembler has resolved `addi x1, x2, -16` to the corresponding form
and normalized its operands to integer bindings:

```c
#include "riscv64.i"

dsymspec_encode_status encode_example(uint8_t out[4], size_t *size) {
    const dsymspec_decl *entry =
        dsymspec_find(&dsymspec_riscv64, DSYMSPEC_DECL_ENCODER, "addi");
    const dsymspec_binding bindings[] = {
        {"r0", 1, NULL}, {"r1", 2, NULL}, {"r2", -16, NULL}
    };
    return dsymspec_encode(entry ? entry->encoder : NULL, bindings, 3,
                          out, 4, size, NULL, 0, NULL);
}
```

`dsymspec_find` returns the first match; to resolve overloaded forms,
iterate `target->declarations` and consult their operand, syntax and feature
fields. Each encoder exposes its potential `binding_names`; a conditional
recipe may not read all of them. `dsymspec_get` and `dsymspec_at` read named
fields and zero-based list entries. For compile-time enumeration use the profile's
`DSYMSPEC_<PROFILE>_DECLARATIONS(X)` macro, which calls
`X(kind, index, name, record_pointer)` for each declaration.

The header-only encoder writes bytes and reports relocation records into
caller-owned arrays. Relocation offsets are relative to the start of the
encoding. It does not apply relocations or automatically enforce form
selection rules; callers match forms and bind their named inputs.
Unavailable recipes are reported as `DSYMSPEC_ENCODE_UNSUPPORTED`.
For example, the x86 branch recipes include the PC-relative addend: ELF64
uses it in the RELA record, whereas ELF32 REL stores the same addend in
the displacement field. Do not apply that bias again in the object writer.

## Coverage

An `.i` file describes the records declared by its source profile. It is
not a promise of complete ISA or object-format coverage. A successful
generation and C/C++ build verifies that the catalog can be consumed, not
that every instruction, operand combination, dialect or CPU variant has
been validated. An assembler still owns its source processing, diagnostics,
symbol table, layout and output format.

The [AMD64 coverage inventory](tests/amd64/README.md) documents supported
long-mode families, ELF fixups, and remaining extension gaps. The [AArch64
coverage inventory](tests/aarch64/README.md) separates default A64/FP/Advanced
SIMD from optional features. The [RISC-V coverage checklist](tests/riscv64/README.md)
distinguishes canonical RV64GC/RV32GC instructions from optional extensions
and assembler pseudos, and documents binding conventions and profile gating.

In particular, PTX is a virtual instruction language, not a mapping to a
particular GPU's native instruction bytes. DXIL is an LLVM-based IR in a
container, not a native ISA. The `nvptx` and `directx` profiles are
metadata-only. AMDGPU recipes target gfx900, Hexagon recipes describe
single-instruction packets, and BPF recipes are little-endian. WebAssembly
and SPIR-V recipes emit instruction bytes or words, not complete modules.
