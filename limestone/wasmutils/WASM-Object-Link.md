# Static wasm32 object linking

`wasm-link` automatically selects object linking when its positional inputs
contain WebAssembly `linking` or `reloc.*` custom sections. Use the original
object bytes: relocation offsets refer to the original binary layout, which
assembling, disassembling, or re-encoding an object may change.

```sh
clang --target=wasm32-unknown-unknown -ffreestanding -O0 -c tests/object_caller.c -o caller.o
clang --target=wasm32-unknown-unknown -ffreestanding -O0 -c tests/object_provider.c -o provider.o
wasm-link --export run --export __wasm_call_ctors caller.o provider.o > linked.wasm
wasm-run linked.wasm run
```

The C++ entry point is `core::link_objects` in `WASM-Object-Link.hpp`. It
returns a validated, finalized `core::Module`, which `core::encode_module`
can write. Finalized modules keep the existing `core::link_modules` behavior;
object/finalized mixtures and `NAME=OBJECT` are not supported. Object linking
needs no external tool; Clang is only needed to create the example inputs.

## Supported conventions

- The [WebAssembly tool-conventions](https://github.com/WebAssembly/tool-conventions/blob/main/Linking.md)
  version-2 `linking` section: symbols, data segment alignment, init functions,
  and optional `wasm32` target architecture. Local symbols remain private;
  cross-object definitions resolve by symbol name, with signature/type
  checking. Strong definitions beat weak ones; the first weak definition
  wins among otherwise equal candidates. Duplicate strong definitions error.
- Unmatched strong functions and Wasm globals remain host imports with their
  original module and field. A referenced undefined strong **data** symbol
  errors because no host-side address can be assigned. Undefined weak data or
  function addresses become zero; a direct call or global access to an
  unresolved weak symbol errors.
- Active, aligned data segments are laid out in input order after the low
  1 KiB of linear memory. The linker supplies a single `env.__linear_memory`
  (exported as `memory`) and, when needed, a mutable `env.__stack_pointer`
  pointing to the top of a 64 KiB stack above static data. The conventional
  `env.__indirect_function_table`, if needed, is shared. Table slot zero
  is reserved; old LLVM-style active function elements at offset one and
  function-address relocations populate subsequent slots.
- Existing Wasm exports and symbols marked `EXPORTED` are preserved. Defined
  non-hidden functions and Wasm globals are exported, but default Clang C
  symbols are usually hidden. Use `--export SYMBOL` or Clang's
  `__attribute__((export_name("name")))`. A requested data symbol is exported
  as an immutable i32 global containing its linear-memory address.
- Init functions are called in ascending priority (ties in input order) by
  `__wasm_call_ctors`. The dispatcher is not run automatically and is not
  automatically exported. Use `--export __wasm_call_ctors` and invoke it once
  before using exports that rely on initialization.

## Relocations

The numeric section index, not the suffix of `reloc.*`, identifies the target.
Supported types are:

| Target | `R_WASM_` types |
| --- | --- |
| CODE | `FUNCTION_INDEX_LEB`, `TABLE_INDEX_SLEB`, `MEMORY_ADDR_LEB`, `MEMORY_ADDR_SLEB`, `TYPE_INDEX_LEB`, `GLOBAL_INDEX_LEB`, `TABLE_NUMBER_LEB` |
| DATA | The same LEB types, plus `TABLE_INDEX_I32`, `MEMORY_ADDR_I32`, `GLOBAL_INDEX_I32`, `FUNCTION_INDEX_I32` |

Relocations are checked against the original section, symbol kind, and
operand or data segment bounds. LEB relocation cells must be five-byte padded
encodings. The final module uses canonical instruction encodings.

## Limits

The linker emits Core 2.0, non-relocatable wasm32. It does not implement
archives, GCC/LLVM LTO, shared libraries, position-independent code, TLS,
common symbols, COMDAT, tag/exception relocations, wasm64, or host-provided
object memory/tables. Use conventional clang output (no `-fPIC`, no debug
relocations). Custom sections, including debug data, are discarded; relocation
sections targeting custom sections and relative/section/offset relocations
are rejected. `target_features` is checked for supported Core 2.0 features and
cross-input conflicts, not emitted.

There is no dead-stripping or string/segment merging. Input start sections,
passive data/element segments, indexed segment instructions, object-defined
memories or tables, and multiple symbol aliases for a single Wasm entity
are not supported. Object global initializers cannot read a Wasm global that
resolves to a definition (Core 2.0 only lets constant expressions read
imported immutable globals). The minimal layout does not supply a CRT, allocator,
WASI runtime, or synthetic symbols such as `__heap_base`.
