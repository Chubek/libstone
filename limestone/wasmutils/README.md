# Limestone WASM utilities

`Limestone::wasmutils` is a C++20 library and command-line tool suite for
WebAssembly Core 2.0, WAT, and WIT. The tools take an input path (`-` for
stdin), write to stdout, and report errors on stderr with a nonzero exit
status. The older [`WASM.aml`](WASM.aml) raw instruction profile still works.

## Core modules

```sh
cmake -S limestone -B build -DLIMESTONE_BUILD_TOOLS=ON
cmake --build build --target wasm-assemble wasm-disassemble wasm-link wasm-validate wasm-run

printf '%s\n' '(module (func (export "add-one") (param i32) (result i32) local.get 0 i32.const 1 i32.add))' \
  | build/wasmutils/wasm-assemble - > module.wasm
build/wasmutils/wasm-validate module.wasm
build/wasmutils/wasm-disassemble --wat module.wasm > module.wat
build/wasmutils/wasm-run module.wasm add-one i32:41
# i32:0x0000002a
```

`wasm-assemble` handles Core text modules, not WAST test scripts or Component
Model text. It resolves symbolic indices and accepts flat and folded
instructions. `wasm-disassemble --wat` prints validated, index-based WAT.
Custom sections have no Core text equivalent and are not printed. The binary
API preserves their names, contents, and relative order and emits them after
the standard sections. Binary parsing checks framing and instruction syntax;
`core::validate_module` checks module and instruction typing separately.

### Static module linking

```sh
build/wasmutils/wasm-link main=main.wasm math=math.wasm > linked.wasm
```

In this example, imports from `"math"` bind to exports in `math.wasm`.
Unmatched namespaces remain host imports; an import from a named input must
match an export. Same-named exports from different named inputs are qualified
as `NAME.FIELD` in the flat output. Alternatively, all inputs can be paths
without names; this mode resolves by export field alone and requires unique
export names. The linker remaps Core indices, merges segment lists, and
combines start functions in input order. It does not emulate separate module
instantiation: all segments initialize before those starts run, and the
output must respect Core 2.0's single-memory rule. Custom sections are not
carried over. Dynamic-linking metadata is rejected.

### Relocatable objects

`wasm-link` recognizes positional wasm32 object files by their `linking` and
`reloc.*` metadata. Object inputs cannot be mixed with finalized modules.
Use `--export SYMBOL` to expose an additional definition:

```sh
clang --target=wasm32 -ffreestanding -c main.c math.c
build/wasmutils/wasm-link --export run main.o math.o > linked.wasm
```

For the C++ API, use `core::link_objects` with the original object bytes.
Relocation offsets refer to those bytes and cannot be recovered by
re-encoding a decoded module. See [object linking](WASM-Object-Link.md) for
supported relocations, memory layout, constructor handling, and limits.

### Interpreter

`wasm-run [--fuel N] INPUT EXPORT [TYPE:BITS ...]` instantiates a binary
module, runs its start function if present, and calls a named function with
typed arguments. Integers accept decimal or hexadecimal bit patterns (and
signed values); floating-point inputs use raw IEEE bits; `v128` uses 32
hexadecimal digits after `0x`. References can be supplied as `funcref:null`
or `externref:null`. The CLI supplies no host imports.

The C++ `core::Instance` API binds host functions and typed memory, table,
and global imports by `(module, name)`. `Instance::with_imports` takes
`HostImports`; the legacy constructor still accepts function bindings alone.
An instance retains memory, tables, globals, and segment state across invocations.
It handles control flow, direct and indirect calls, multi-value returns,
scalar numeric instructions, fixed-width SIMD, and checked memory and table
access. The host can read and write named exports using the checked memory,
table, and global accessors. Each call is bounded by fuel and configurable
stack-frame and local limits. The current dense backing stores limit each
instance to 64 MiB of linear memory and 1,048,576 table entries total.
Imported objects share storage with the host and observe guest writes and
growth. Externally serialize all handles and instances sharing an object.
Imported `funcref` tables and mutable `funcref` globals can only be bound to
one live instance; their function references are cleared when that instance
ends. A trap does not generally roll back earlier state changes.

```cpp
namespace core = limestone::wasmutils::core;
core::HostMemory memory(core::MemoryType{{1, 4}});
const core::HostObjectImport objects[] = {{"host", "memory", memory}};
auto instance = core::Instance::with_imports(std::move(module), {.objects = objects});
```

## Core 2.0 binary instructions

[`WASM-Core.hpp`](WASM-Core.hpp) provides `core::lookup`, `core::decode`,
and `core::encode` for all 437 binary forms in the
[W3C Core 2.0 instruction index](https://www.w3.org/TR/2025/CRD-wasm-core-2-20250616/#index-instr),
counting separate encodings for `select` and the `else`/`end` delimiters. It
includes the 236 fixed-width SIMD forms, bulk memory and table operations,
reference types, floating point, and structured control instructions. It
rejects unassigned encodings and extensions outside Core 2.0, such as atomics,
relaxed SIMD, and GC.

The codec accepts valid padded LEB128 and writes canonical LEB128, preserving
floating-point payloads and SIMD bytes. It does not check operand types, index
ranges, memory alignment, or lane bounds. `core::check_expression` checks
delimiter nesting and the terminal `end`. `core::decode_code_section`
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

`--expression` and `--module` emit diagnostic listings, not WAT. Without a
mode the CLI decodes a raw sequence without checking nesting. To refresh the
checked-in inventory, download the dated W3C HTML linked above and run
`python3 limestone/wasmutils/tools/generate_core_opcodes.py PATH_TO_HTML`.
`--check` verifies it without writing. The normal build needs neither Python
nor the spec download.

## ANSI C generation

`wasm2c`, `wat2c`, and `wit2c` emit ANSI C (C89/C90), without a C++ runtime
dependency. `wasm2c` accepts binary modules or legacy raw streams; `wat2c
--module` uses the module backend and plain `wat2c` retains the legacy text
interface.

The module backend supports multiple results, locals, structured control
flow, direct and indirect calls, scalar numeric operators, references, host
function imports, defined memories, globals and tables, and start functions.
It still rejects non-function imports and SIMD.
Generated floating-point arithmetic uses the host math library (link with
`-lm` where required). It expects IEEE binary32/binary64, round-to-nearest
ties-to-even, gradual underflow, and disabled floating-point traps. Do not
compile it with fast-math.

For persistent state, use `wasm_instance_create`, `wasm_instance_invoke`,
and `wasm_instance_destroy`. Use `wasm_instance_create_with_imports` to
supply host function callbacks. The generated translation unit lists the
status codes and provides checked accessors for exported memory, tables,
and globals. Invocations are fuel-bounded. A trap or insufficient output
capacity does not roll back writes.

For one-off calls, the generated translation unit keeps the compatibility
wrapper:

```c
#include <stddef.h>
struct wasm_result { unsigned width; unsigned long lo, hi; };
int wasm_module_invoke(const char *name, size_t name_size,
                       const struct wasm_result *args, size_t nargs,
                       struct wasm_result *out, size_t capacity, size_t *count,
                       unsigned long fuel);
```

The wrapper creates a fresh instance without host callbacks on each call.
The export name is a byte string of length `name_size`. Numeric values are
passed by bits in 32-bit limbs; `hi` is zero for 32-bit values. Return values are `0` for
success, `1` for a trap or execution limit, `2` for insufficient output
space, and `3` for an unknown export or invalid arguments. No results are
written on failure. `count`, if non-null, must not alias `args` or `out`; it
receives the result count on success or insufficient capacity, and zero
otherwise. `args` and `out` may overlap.

### Legacy raw streams

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

Without the Wasm magic, `wasm2c` reads a **raw instruction stream**, without
sections or a terminating `end` instruction. The legacy tools support all
36 forms in `WASM.aml`: integer constants (`-64..63`, as canonical
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

`wit2c` generates C90 host declarations from a WIT 0.2 source file. The
parser models interfaces, worlds, uses and includes, and validates local
references. The emitter supports scalar and structural types, including
records, variants, enums, flags, strings, lists, tuples, options, results,
and opaque resource handles. It emits declarations for world imports and
exports. Supply other WIT packages explicitly with repeatable `--dep PATH`
options. Qualified references resolve transitively by exact package and
version; there is no directory scanning or downloading.

```sh
build/wasmutils/wit2c --dep types.wit --dep io.wit app.wit > app.h
```

`wit2c` does not generate function implementations or manage resource
lifetimes. Its default C headers use native host types, not the Canonical ABI.
Use `wit2c --canonical` to inspect the Core signatures and memory layouts
for synchronous memory32/UTF-8 functions. The C++ API in
`WIT-Canonical.hpp` adapts calls in both directions and checks values
crossing the Core/component boundary. It does not load or link component
binaries or support async calls, resource handles, or other string encodings.

Integers, booleans, characters, and floating-point scalars map to host types.
In particular, s64/u64 use high/low 32-bit limbs, booleans use `int` (0 or 1),
and strings and lists use pointer/length views. The header checks the
required integer widths and signed ranges with `<limits.h>` and does not
define a binary wire format. Unqualified `arithmetic.add` is declared as
`wit_arithmetic_W_add`.

## Embedding

```cmake
find_package(Limestone CONFIG REQUIRED COMPONENTS wasmutils)
target_link_libraries(my_tool PRIVATE Limestone::wasmutils)
```

The module API is in `limestone::wasmutils::core`:

| Header | Use |
| --- | --- |
| `WASM-Core.hpp` | Binary instruction codec |
| `WASM-Module.hpp`, `WASM-Validate.hpp` | Binary module codec and validation |
| `WAT-Module.hpp`, `WAT-Print.hpp` | WAT assembly and printing |
| `WASM-Link.hpp`, `WASM-Instance.hpp` | Static linking and interpretation |
| `WASM-Object-Link.hpp` | Relocatable wasm32 object linking |
| `WASM-CC.hpp` | Standalone ANSI C source generation |
| `WITParser.hpp`, `WIT-IDL.hpp` | WIT AST, validation, and C header generation (in `limestone::wasmutils`) |
| `WIT-Canonical.hpp` | Synchronous Canonical ABI layout and call adapters (in `limestone::wasmutils::canonical`) |

Include these as `<wasmutils/HEADER>`. Legacy entry points remain in
`WASM-Binutils.hpp`, `WASM-Runtime.hpp`, and `WATCompiler.hpp`. The `.g` and
`.asdl` files document syntax and data shapes; the implementation does not
build generated parsers.

## Tests

With `LIMESTONE_BUILD_TESTS=ON`, build the `wasmutils-check` target to build
only these tools and tests and run the `wasmutils` CTest suites:

```sh
cmake -S limestone -B build -DLIMESTONE_BUILD_TESTS=ON
cmake --build build --target wasmutils-check
```

For independent round-trip checks, run the optional
[`check_core2_roundtrip.py`](tools/check_core2_roundtrip.py) against a
[WebAssembly/spec `wg-2.0` checkout](https://github.com/WebAssembly/spec/tree/wg-2.0).
It verifies byte-identical reassembly and compilation in Node.js and can
also test upstream invalid-text fixtures:

```sh
python3 limestone/wasmutils/tools/check_core2_roundtrip.py \
  path/to/spec-core2 build/wasmutils build/core2-roundtrip --check-invalid
```
