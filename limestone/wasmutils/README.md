# Limestone WASM utilities

`Limestone::wasmutils` is a C++20 library with a WebAssembly Core 2.0 binary
instruction codec and a separate interpreter and ANSI C90 backend for the
selected straight-line profile in [`WASM.aml`](WASM.aml). The tools take an
input path (`-` for stdin), write to stdout, and report errors on stderr with a
nonzero exit status.

## Core 2.0 binary instructions

[`WASM-Core2.hpp`](WASM-Core2.hpp) provides `core2::lookup`, `core2::decode`,
and `core2::encode` for all 437 binary forms in the
[W3C Core 2.0 instruction index](https://www.w3.org/TR/2025/CRD-wasm-core-2-20250616/#index-instr),
counting separate encodings for `select` and the `else`/`end` delimiters. It
includes the 236 fixed-width SIMD forms, bulk memory and table operations,
reference types, floating point, and structured control instructions. It
rejects unassigned encodings and extensions outside Core 2.0, such as atomics,
relaxed SIMD, and GC.

The codec accepts valid padded LEB128 and writes canonical LEB128, preserving
floating-point payloads and SIMD bytes. It does not check operand types, index
ranges, memory alignment, or lane bounds. `core2::check_expression` checks
delimiter nesting and the terminal `end`. `core2::decode_code_section`
extracts local declarations and instruction streams from defined function
bodies in a binary module; it skips other sections and does not validate the
module.

```sh
cmake -S limestone -B build
cmake --build build --target wasm-disassemble

# Raw instruction expression: i32.const 128, end (octal bytes)
printf '\101\200\001\013' | build/wasmutils/wasm-disassemble --expression -

# Print the code section of a binary module, with module-relative offsets
build/wasmutils/wasm-disassemble --module input.wasm
```

The CLI listing is diagnostic text, not round-trippable WAT. Without either
option the CLI decodes a raw sequence without checking nesting. To refresh the
checked-in inventory, download the dated W3C HTML linked above and run
`python3 limestone/wasmutils/tools/generate_core2_opcodes.py PATH_TO_HTML`.
`--check` verifies it without writing. The normal build needs neither Python
nor the spec download.

## ANSI C generation and the `WASM.aml` profile

`wasm2c`, `wat2c`, and `wit2c` emit ANSI C (C89/C90), without a C++ runtime
dependency. The new Core 2.0 codec does not extend execution or C lowering to
all instructions.

```sh
cmake -S limestone -B build
cmake --build build --target wasm2c wat2c wit2c

printf '%s\n' '(i32.add (i32.const 20) (i32.const 22))' \
  | build/wasmutils/wat2c - > answer.c
cc -std=c90 -pedantic-errors -c answer.c

# i32.const 20, i32.const 22, i32.add (octal bytes)
printf '\101\024\101\026\152' | build/wasmutils/wasm2c - > answer.c

printf '%s\n' 'interface arithmetic { add: func(a: u32, b: u32) -> u32; }' \
  | build/wasmutils/wit2c - > arithmetic.h
```

`wasm2c` takes a **raw instruction stream**, without the Wasm magic,
sections, or a terminating `end` instruction. `wasm2c`, `wat2c`, and the
interpreter support all 36 forms in `WASM.aml`: integer constants (`-64..63`, as canonical
single-byte signed LEB128), the listed integer arithmetic/bitwise/count/shift
operations, `drop`, `select`, `nop`, and `unreachable`. Module serialization,
general LEB128, structured control flow, imports, exports, locals, calls, memory
instructions, and floats are outside the execution profile. Unsupported
forms are rejected rather than silently ignored.

Raw streams start with an empty operand stack and can leave zero or more i32/i64
values. Raw encoding/decoding only checks instruction encodings; `validate`,
`execute`, `compile_wat`, and `emit_c` check operand types and stack effects.
`unreachable` unconditionally traps; later instructions still undergo validation.
The raw linker combines fragments without symbols or relocations.

`wat2c` accepts flat instructions, folded expressions, and a restricted text
wrapper with one anonymous, zero-argument function, such as:

```wat
(module
  (func (result i32)
    (i32.add (i32.const 20) (i32.const 22))))
```

This wrapper provides a text function and its result signature, not support for
binary Wasm modules. Line comments (`;;`) and nested block comments (`(; ... ;)`)
are supported. Decimal and hexadecimal constants can use digit separators
(`_`). Folded expressions and block comments each have a nesting limit of 128.

The generated C translation unit exports:

```c
#include <stddef.h>
struct wasm_result { unsigned width; unsigned long lo, hi; };
int wasm_run(struct wasm_result *out, size_t capacity, size_t *count);
```

Results are written bottom-first when `wasm_run` returns `0`. `width` is 32
or 64, `lo` and `hi` each hold 32 bits, and `hi` is zero for i32. A trap
returns `1`; a missing buffer or insufficient capacity returns `2`. Neither
failure writes to `out`. The optional `count` receives the required length
(zero on a trap) and must not alias `out`. The buffer may be null for programs
with no results.

## WIT

`wit2c` generates C header declarations for scalar interface functions. It
does not generate their implementations or a Component Model Canonical ABI
adapter. Component linking and ownership, resources, worlds, lists, and
structural types are outside its scope.

Input can declare an optional `package namespace:name@major.minor.patch;`,
followed by one or more interfaces. Functions take named scalar parameters and
return either one scalar or nothing. The emitted header defines host mappings
for WIT integer types, `bool`, `char`, `f32`, and `f64`. In particular, the
64-bit integer types use high/low 32-bit limbs, booleans use `int` (0 or 1),
and floats use native `float` and `double`. The header checks the required
integer widths and signed ranges with `<limits.h>`; it does not specify a
binary wire format. For example, the unqualified `arithmetic.add` above is
declared as `wit_arithmetic_W_add`.

## Embedding

```cmake
find_package(Limestone CONFIG REQUIRED COMPONENTS wasmutils)
target_link_libraries(my_tool PRIVATE Limestone::wasmutils)
```

The API is in `limestone::wasmutils`. Include `<wasmutils/WASM-Core2.hpp>`
for the Core 2.0 codec, `<wasmutils/WASM-Binutils.hpp>` for that codec and the
legacy profile's raw instruction tools, `<wasmutils/WASM-Runtime.hpp>` for execution,
`<wasmutils/WASM-CC.hpp>` for C generation, `<wasmutils/WATCompiler.hpp>` for
WAT lowering, and `<wasmutils/WIT-IDL.hpp>` for the WIT model and C declarations.
The separate checked wasm32 linear-memory helper is not connected to any
instruction in this profile. The `.g` and `.asdl` files describe the accepted
surface and data shapes; the implementation does not build generated parsers.
