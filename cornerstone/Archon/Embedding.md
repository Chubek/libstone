# Embedding Archon

Archon has two embedding layers. The Python and C++ library APIs cover
architecture compilation, database operations, instruction encoding and
execution, and generation. Generated codecs, emulators, and IR libraries
provide their own C runtime APIs and do not embed Python.

| Task | Interface |
| --- | --- |
| Compile `.arch`, load or inspect ArchDB, assemble or interpret instructions | Core Python or C++ API |
| Discover IR backends and stages, generate artifacts, publish a generated directory | Separate IRGen API |
| Execute modeled instructions without Python | Generated codec/emulator C API ([guide](Emulator.md)) |
| Compile MachineIR in a deployed application without Python | Generated C backend below |
| Execute a supported host ELF in a restricted child | Generated `RunSandbox.c` ([guide](IRTools.md#running-a-linked-program)) |

## Python

Use `archon.api` for the compiler and runtime, and `archon.irgen.api` for
IRGen. From `cornerstone/Archon` or with that directory on `PYTHONPATH`:

```python
from archon import api
from archon.irgen import api as irgen

db = api.compile_file("stdspec/riscv/rv32i.arch")
target = api.Target(db)
name, operands = target.assemble("addi x1, x0, 42")
assert target.encode(name, operands).hex() == "9300a002"
state = target.execute_state(name, operands, target.new_state())
assert state.registers[1] == 42

files = irgen.generate_ir("anf", [db], options={"oracle": False})
irgen.write_generated_directory(files, "generated")
```

The core API also exposes `validate_source`, structured database inspection,
ArchDB load/save/parse/dump, and C99 codec and emulator generation. `Target`
covers single-instruction and whole-program operations. The IRGen API
includes standard targets, representation and stage discovery, individual
stage emission, the macro ABI, and the optional native ELF bridges. On either
module, `exports()` lists supported names and `get_export()` and `dispatch()`
give access to that registry, not to arbitrary Python modules.

IRGen returns source files; it does not compile or run artifacts on
generation. The optional `assemble_object` and `link_executable` bridges are
different: they deliberately invoke configured external tools.

## C++17

The C++ API runs the same implementations in-process through CPython. Building
the optional library requires CMake and CPython 3.10+ development headers and
the embedding library for a GIL-enabled build. It is independent of the
generated backend C library.

From `cornerstone/Archon`, build and install into a prefix you control:

```sh
cmake -S . -B build-embedding
cmake --build build-embedding
cmake --install build-embedding --prefix "$HOME/.local/archon"
```

A separate application can set `CMAKE_PREFIX_PATH` to that prefix and
link the installed target:

```cmake
find_package(Archon CONFIG REQUIRED)
add_executable(example example.cpp)
target_link_libraries(example PRIVATE Archon::Archon)
```

Pass the package root reported by CMake as `Archon_PYTHON_DIR` and a path to
an `.arch` file to the example below. The directory containing the
`archon` package in the checkout can also be used as a package root.

```cpp
#include <archon/Archon.hpp>
#include <archon/Archon-IRGen.hpp>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    archon::Interpreter python(argv[1]);
    auto db = archon::compile_file(argv[2]);
    archon::Target target(db);
    auto inst = target.assemble("addi x1, x0, 42");  // RV32I input
    auto result = target.execute_state(inst, target.new_state());
    if (result.registers().at(1).as_uint64() != 42) return 1;
    auto files = archon::irgen::generate_ir("anf", {db}, {{"oracle", false}});
    archon::irgen::write_generated_directory(files, "generated", {argv[2]});
}
```

Use one main interpreter per process. An application that already owns
CPython should not create `archon::Interpreter`; register the trusted
package root with `archon::add_package_path` before the first API call.
Construct and destroy an owned interpreter on the same thread. Destroy all
Archon objects and join their calling threads before finalizing Python.
`archon::Value` is a reference-owning handle; copies refer to the
same Python object. The wrappers acquire the GIL for their operations. Python
exceptions cross the API as `archon::Error`.

If the host has already registered a private parent package containing
`archon`, `archon::PackageScope` routes facade imports through that parent on
the current thread without changing `sys.path`. Create and destroy nested
scopes in reverse order on the same thread.

Both APIs require the caller to list protected input paths when publishing
a generated tree, especially when using forced replacement. Include any
ArchDB, script, or bank path located in or aliased into the destination.

## Generated native backend

Generate and build once per target. This example uses RV64IM:

```sh
./tools/archonc stdspec/riscv/rv64im.arch -o rv64im.archdb
./tools/archon-irgen gated rv64im.archdb --target RISCV64IM \
    --profile native --abi LP64 --no-oracle -o generated
cmake -S generated -B build
cmake --build build
build/archon-backend --input program.mir >program.s
build/archon-backend --input program.mir --pic >program-pic.s
build/archon-backend --input program.mir --schedule-pure --emit-physical >program.physical
build/archon-backend --input function.mir --emit-code >function.bin
```

For the base RV64I profile, compile `stdspec/riscv/rv64i.arch` and select
`--target RISCV64` instead. The ABI stays LP64; `BackendManifest.json` records
the qualified ISA, and lists the runtime MIR operations unavailable on RV64I.
See [native profile coverage](stdspec/riscv/README.md#rv64-native-profiles).
For AMD64 and AArch64, see the [scalar GAS profiles](NativeScalarGAS.md).

The generated `archon_backend` static library exposes `Backend.h`. Link it
into an application compiled as C99 or C11. `archon_backend_compile` compiles a
caller-owned `ArchonMIRModule`; `archon_backend_compile_text` decodes the
[runtime text format](MachineIR.md). Both borrow input and options only until
return. Callers must provide valid array storage for the duration of the call,
including when invoking from another language through an FFI. Input views and
referenced memory must not be concurrently mutated.

On RV64, the `_ex` compile variants additionally accept `ARCHON_BACKEND_SCHEDULE_PURE`
with either `ARCHON_BACKEND_PHYSICAL_RECORDS` or `ARCHON_BACKEND_MACHINE_CODE`.
`ARCHON_BACKEND_PIC` is assembly-only and selects `.option pic` plus GAS `la`
address materialization; combining it with either non-assembly output mode is
invalid. The output modes are mutually exclusive.
`archon_backend_compile_physical` is the typed convenience API for physical
inspection with default ordering.
The AMD64 and AArch64 profiles reject those three RV64-specific options.
AMD64 and AArch64 both support assembly-only PIC lowering with ELF GOT and PLT
relocations.

Initialize an `ArchonBackendArtifact` to zero and reuse it only after calling
`archon_backend_artifact_destroy`. Successful textual output is NUL-terminated
and `size` excludes the terminator. Treat raw machine code as bytes of that
size, not as a string. On failure no buffer is published; diagnostics report
the relevant stage and available indices. Options support a cooperative
cancellation callback and an artifact byte budget. No global configuration,
intermediary files, subprocesses, or Oracle requests are used. Independent
calls may run concurrently when their caller-owned objects and callbacks are
independent.

`cmake --install build --prefix PREFIX` installs the public header, library,
driver and manifest. CMake consumers using the prefix can
`find_package(ArchonBackend CONFIG REQUIRED)` and link `Archon::Backend`.
Gated SSA generation also provides `GatedBackend.h` and `Archon::GatedBackend`
for the verified single-block scalar adapter with an explicit return value.
Typed TAC generation provides `TypedTACBackend.h` and `Archon::TypedTACBackend`
for its bounded scalar control-flow adapter, including branches and phi inputs
on loop backedges. It accepts 64-bit word parameters and word or void returns,
plus internal booleans. It does not lower calls, memory, or effects. Both
adapters have `_compile_ex` entry points taking the native flags. Other
representations retain their existing interfaces. Use a separate install
prefix for each generated configuration; the package name and `IR.h` install
path are shared, but the `IR.h` content is representation-specific.

For the legacy embedding facade and `Program.i` build flow, see
[IRPipeline.md](IRPipeline.md#c11-embedding-api).
