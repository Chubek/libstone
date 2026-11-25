# `std.assembler`

Import `std.assembler` to assemble text inside a generated tool using
Sandstone's C embedding API. It does not start `sandstone-as` or create
temporary source or output files.

| Function | Selects |
| --- | --- |
| `assemble(source: String, bank: String, syntax: String) -> Vec<Int>` | A `.symas` bank path, relative to the tool's working directory unless absolute. |
| `assembleTarget(source: String, target: String, syntax: String) -> Vec<Int>` | A built-in Sandstone profile, for example `rv64im` or `amd64`. Requires a Sandstone build with `SANDSTONE_ASSEMBLER_DSYMAS=ON` (the default). |

An empty bank or target selects the SDK's build default profile. An empty
syntax keeps the bank or profile default. For example:

```text
.language "AssembleAdd"
.import std.assembler as A

.rule Start
'syntax: EOF
'semantics:
let bytes = A:assembleTarget("addi x1, x0, 7\n", "rv64im", "default");
return len(bytes);
.end
```

The vector is `[147, 0, 112, 0]`, so the example prints `4`. C tools currently
require a scalar start-rule result; actions can still use the vector. Each
result is a fresh vector of octets (`0..255`), and a source that emits nothing
returns `[]`. Flat layout, including
any alignment padding and BSS, follows Sandstone. Assembly errors, invalid
banks, targets or syntaxes, unresolved relocations, and flat image failures
produce a `SANDSTONE` diagnostic. There is no object output or linking; the
returned bytes are not executed.

Each call opens and closes its own assembler context. Source, selector and
syntax are NUL-free text. There is no timeout or sandbox: trust the assembly
source and banks. Sandstone source can contain file includes and Lua blocks.
The SDK limits flat images to 64 MiB; callers should still account for the
memory required to copy bytes into a vector.

## Optional native dependency

The generator needs no native SDK to read the import or check a specification.
Generated Python 3, C99 and C11 tools can use the library. Tools without the
import do not need the companion or Sandstone. The existing `.sandstone … bank`
declaration is unchanged; it still uses its own [interop bridge](interop.md).

With a Sandstone SDK install containing the current `Sandstone::assembler` CMake
target, configure the focused companion:

```sh
cmake -S cornerstone/LLSynth/stdlib/assembler -B build/llsynth-assembler \
    -DCMAKE_PREFIX_PATH=/path/to/sandstone/install
cmake --build build/llsynth-assembler
```

For generated Python, put the library under `LLSYNTH_LIBRARY_PATH` or the
platform loader's search path. Set `LLSYNTH_ASSEMBLER_LIBRARY` to an absolute
shared-library path to override both. The
library is loaded lazily on the first assembly call. Linux defaults to
`libllsynth_assembler.so`, macOS to `libllsynth_assembler.dylib`, and Windows
to `llsynth_assembler.dll`.

For C, use the include path for `llsynth_assembler.h` and link the shared
library. A CMake consumer can add the companion directory with `add_subdirectory`
and link to `LLSynth::assembler`; for a manual Linux build:

```sh
cc -std=c11 -I cornerstone/LLSynth/stdlib/assembler example.c \
    -L build/llsynth-assembler -Wl,-rpath,"$PWD/build/llsynth-assembler" \
    -llsynth_assembler -lm -o example
```

## Tests

Configure against the same SDK with built-in profiles to test generated
Python and native tools against real assembly, with no assembler executable
on `PATH`:

```sh
cmake -S cornerstone/LLSynth/tests/assembler -B build/llsynth-assembler-tests \
    -DCMAKE_PREFIX_PATH=/path/to/sandstone/install
cmake --build build/llsynth-assembler-tests
ctest --test-dir build/llsynth-assembler-tests --output-on-failure
```

The regular LLSynth runner includes the module’s check-time tests and skips
native cases unless the focused SDK build supplies the companion and test bank
generator.
