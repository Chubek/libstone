# Chapter 26: Extension Guide

Add a reader by implementing the `ObjectFile` contract, register a target, add fixtures, then expose queries and documentation. Keep ABI additions source compatible.

## Linker directive and macro SDK (v1)

`ld/ScriptSDK.hpp` provides an owning C++20 source-extension registry:

```cpp
qld::ScriptSDK sdk;
sdk.defineMacro("symbol", {"name", "value"}, "${name} = ${value};\n");
sdk.registerDirective("answer", [](const qld::ScriptSDK::Arguments &args) {
    if (!args.empty()) throw std::runtime_error("answer takes no arguments");
    return std::string("@symbol answer, 42\n");
});
qld::Options options;
options.useScriptText = true;
options.scriptText = sdk.preprocess("@answer\nSECTIONS { .meta : { LONG(answer) } }");
auto result = qld::link(options);
```

Include both `ScriptSDK.hpp` and `ld.hpp` (and `<stdexcept>`). Macro substitution
uses `${parameter}`; callback arguments split on commas outside quotes and
balanced brackets. Generated source is expanded recursively. Invalid names,
duplicate registrations, unknown directives, wrong arity, recursion and output
limits raise diagnostics. Registries are per instance and have no global output
buffer. They must not be mutated concurrently with expansion.

The C API in `ld.h` exposes `qld_register_directive`, `qld_define_macro`, and
`qld_expand_script`. Registrations attach to a `qld_linker_t`; `qld_run` and
`qld_run_bytes` apply them. Callback user data remains borrowed for the linker's
lifetime. Callbacks write source through the provided sink and return zero on
success; return nonzero and populate the error buffer on failure. Sink failures
are sticky. Release returned source/errors with `qld_free_string` and images
with `qld_free_bytes`. Existing `qld_options_t` layout is unchanged.

The [C11 example](../ld/examples/linker-sdk.c) generates a native Linux x86-64
program with a custom macro. From the Yellowstone directory:

```sh
cmake -S . -B build -DQOBJFILE_DOMTERM=OFF
cmake --build build --parallel
cmake --install build --prefix /tmp/opencode/yellowstone-install
P=/tmp/opencode/yellowstone-install
cc -std=c11 -Wall -Wextra -Werror -I"$P/include" -c ld/examples/linker-sdk.c -o "$P/example.o"
c++ "$P/example.o" -L"$P/lib" -lqld -llibstone_script_lua -ldl -lm -o "$P/example"
"$P/example" "$P/program"
"$P/program"  # exits with status 7
```

In-tree CMake consumers link `qobjld::qobjld`; bundled Lua propagates through
the target. Installed static consumers link the libraries shown above; Lua and
Kaguya headers are not part of the public SDK. `YELLOWSTONE_BUILD_BINSCRIPT`
defaults OFF because this checkout has no binscript sources; enabling it
requires those sources and diagnoses their absence.

## Extending ISA support

ISA support is a horizontal slice shared by tools and libraries:

1. Add or update a processor description under `infobank/isa`.
2. Validate it against `infobank/isa.peg` and the field conventions in `infobank/schema.json`.
3. Load it through `qisa::Database` and inspect the resulting `Document` AST.
4. Add architecture-name mapping in the consumer, such as `dump/main.cpp`.
5. Add a fixture containing representative encodings and verify fallback output for unknown encodings.

A consumer should treat `syntax` and `semantics` as optional metadata. The object reader remains useful when no description is installed, and a decoder must print raw bytes when an encoding cannot be resolved.

## Adding a new architecture

Extend the qbfd `Arch` enum and backend mapping first. Then add the corresponding ISA filename mapping, profile reporting, and disassembly width rules. Keep register names and aliases in the `.isa` file so other consumers can reuse them.

## Keeping the slice reusable

Do not put terminal rendering, file-format parsing, or linker policy into `common/isa.hpp`. The qisa AST/database layer should remain independent; qobjdump, qld, and future compiler or relocation tools can build their own views over it.
