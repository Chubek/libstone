# Native C11 and Sandstone interoperability

Interop declarations expose typed functions to semantic actions, `.init`,
ordinary `.function` bodies and lowering actions. Generated Python 3 and C11
tools support them; C99 also works. No compiler, assembler, Python package,
or shared library is invoked or loaded **during generation**. Existing
specifications gain no mandatory external dependency.

## Importing a C-ABI function

```text
.extern c add(a: Int, b: Int) -> Int from "./libexample.so" symbol "example_add"
.extern c message() -> String from "./libexample.so" symbol "example_message"
.extern c notify(value: Int) -> Void from "./libexample.so" symbol "example_notify"
```

`symbol` is optional and defaults to the LLSynth function name. `from` is
required. Argument names and types, a return type, and a non-variadic C ABI
are mandatory. For example, the first function must implement:

```c
#include <stdint.h>
int64_t example_add(int64_t a, int64_t b) { return a + b; }
```

It can then be used directly or as a typed function value:

```text
let fn: Fn(Int, Int)->Int = &add;
return fn(19, 23);
```

| LLSynth | Exact native ABI |
| --- | --- |
| `Int` / `Int64` | `int64_t` |
| `Float64` | `double` |
| `Bool` | `int`, **not** C `_Bool`; zero is false |
| `String` | `const char *`, non-null, NUL-terminated UTF-8 |
| `Void` | `void`, result only |

Type aliases resolving to these types are supported. `Int` may widen to
`Float64` as in ordinary LLSynth calls. Arity and known argument types are
checked at generation time, including indirect calls. Runtime boundary
checks reject dynamically mismatched values before entering native code.
Strings are borrowed for the duration of a call; native code must not retain
or modify argument pointers. Native returned strings must remain valid until
the bridge copies them. The bridge does not free them. Null and invalid
UTF-8 string results are diagnosed as `INTEROP` errors.

### Python 3 tools

Build the example library, then generate and run the tool:

```sh
cc -std=c11 -fPIC -shared example.c -o libexample.so
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret \
  -T python3 -o example.py example.llsynth
python3 example.py
```

The generated tool uses standard-library `ctypes.CDLL` lazily on first call.
For a bare filename such as `libexample.so` or `libexample.so.1`, it searches
the colon-separated `LLSYNTH_LIBRARY_PATH` directories left to right, ignoring
empty entries. Relative entries are relative to the **runtime working
directory**. The first existing regular file with exactly that filename is
passed to the loader by absolute path. If that file cannot be loaded, the
call fails without trying another directory. If no file matches, the original
name is passed unchanged to the normal platform dynamic loader.

Explicit paths in `from`, such as `./libexample.so` or an absolute path, do
not search `LLSYNTH_LIBRARY_PATH` and are passed unchanged to the loader. `./`
is relative to the runtime working directory, not the specification or
generated file. Missing libraries and symbols produce `INTEROP` diagnostics;
an unused import does not load anything. Native handles remain loaded for
the lifetime of the generated Python module.

### C11 tools and split embedding

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret \
  -T c11 -o example.c example.llsynth
cc -std=c11 example.c example-native.c -lm -o example
```

C output emits typed `extern` declarations and direct native calls. Link the
implementation object files or libraries explicitly. **`from` describes the
Python dynamic-library binding; it is not an automatic linker option in C.**
`LLSYNTH_LIBRARY_PATH` does not affect C linker resolution.
The C linker, not a runtime loader, resolves the symbol. Missing symbols
therefore fail the build. Declarations of a single symbol must agree on both
its normalized signature and library, even when assigned multiple action
names. An ABI mismatch with the actual implementation cannot be discovered
from a declaration alone: supply the exact signature above.

This works with `--split=all` and `--driver-mode=library`; the existing public
C embedding API is unchanged. Native state is owned by native code and is
not reset by an LLSynth context reset.

## Assembling through Sandstone

```text
.sandstone assemble bank "./x86.symas" syntax "intel"
```

This declares `assemble(source: String) -> Vec<Int>`. `syntax` is optional
(default: the bank's first syntax). An optional `cli` field comes last:

```text
.sandstone assemble bank "./x86.symas" syntax "intel" cli "/opt/sandstone/bin/sandstone-as"
```

For example:

```text
let bytes: Vec<Int> = assemble("nop\nret\n");
return bytes[0];
```

This invokes **Sandstone's actual assembler**, using the bank's instruction
encoders, operands, syntax, directives, labels and diagnostics. Results are
copied into an ordinary vector, one integer in `0..255` per byte, including
any BSS zero-fill. Empty images are valid. Bank paths are resolved relative
to the runtime working directory. Banks are not bundled with generated
tools; create them using Sandstone's documented spec-compiler workflow.

* **Python 3:** runs `sandstone-as --bank BANK [--syntax SYNTAX] -f bin -o OUTPUT INPUT`
  using an argument vector, never a shell. `cli` defaults to `sandstone-as`
  on `PATH`. Temporary input/output files are cleaned up on success or
  failure. Calls have a 30-second timeout and a 64-MiB image limit.
* **C11:** calls the public API from `sandstone/assembler/assembler.h`
  (`sandstone_asm_new`, `sandstone_asm_set_syntax`,
  `sandstone_asm_assemble_string`, relocation/image getters, and
  `sandstone_asm_free`). Only specifications declaring `.sandstone` emit
  `#include <assembler.h>` and SDK references. Add Sandstone's assembler
  include directory and link its SDK. The optional `cli` value configures
  Python only: C always uses the SDK, not a subprocess. SDK contexts are
  invocation-local and released even on generated allocator failure.

For an SDK consumer, compile generated C as C11 and use Sandstone's exported
`Sandstone::assembler` CMake target for linking (it supplies transitive C++
runtime, Lua and compression dependencies). With manually built archives,
the equivalent Linux commands are:

```sh
cc -std=c11 -I sandstone/assembler -c generated.c -o generated.o
c++ generated.o libsandstone_assembler.a libsandstone.a liblua.a \
  -lz -ldl -lm -o generated
```

Assembly failures, missing banks/CLI, unknown syntaxes, unresolved
relocations, and image failures produce `SANDSTONE` diagnostics rather than
empty success. Both bridges deliberately request **flat binary**, not JSON
object output. There is no implicit linker, executable-memory allocator,
instruction execution, or cross-architecture calling convention conversion.
Assembly defining a callable host function must separately be linked into a
native library and imported through `.extern c` with a matching host ABI.

## Scope and trust

* Current tool targets: Python 3, C99 and C11. C++ generation with any interop
  declaration is explicitly rejected, including under `--check`.
* No pointers, structures, vector ABI, varargs, callbacks, native exceptions,
  calling-convention selection, object-file linking, or automatic exports.
  Unsupported ABI types are rejected.
* Imports are semantic-action calls in the **generated tool**. They do not
  automatically create native calls in programs emitted by a `.backend`.
* Imported functions and assembly source are trusted native inputs.
  Sandstone supports file directives and embedded Lua; this bridge is not a
  sandbox. Native crashes, invalid pointers, ABI mismatches, and indefinite
  native calls cannot be recovered by the language runtime. No timeout is
  imposed on native C calls or the in-process C assembler SDK.

## Tests

```sh
python3 cornerstone/LLSynth/tests/interop/test_interop.py
```

The ordinary suite needs Perl and a C11 compiler, builds a real shared library,
and tests Python/C11 parity, split output, type/arity errors, indirect calls,
UTF-8/null results, dynamic boundary errors and lazy-loading diagnostics.
Real Sandstone tests are opt-in, not mock-based. In a full repository checkout:

```sh
cmake -S cornerstone/LLSynth/tests/interop -B build/llsynth-interop
cmake --build build/llsynth-interop -j4
LLSYNTH_SANDSTONE_BUILD="$PWD/build/llsynth-interop" \
  python3 cornerstone/LLSynth/tests/interop/test_interop.py
```

The small test build uses the actual decoder/assembler sources and bundled
Lua/Kaguya, but no unrelated projects or network downloads. It creates
`sandstone-as`, the SDK archives and a real two-instruction `.symas` bank
builder. Tests check exact instruction/directive bytes and reject invalid
assembly and unresolved relocations through both generated tool targets.
For a sparse checkout, `-DVENDOR_ROOT=/path/to/snapshot` can point at a local
snapshot containing `third_party/lua`, `third_party/kaguya` and
`cmake/KaguyaCompat.hpp`.
