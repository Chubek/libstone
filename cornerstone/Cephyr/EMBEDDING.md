# Embedding Cephyr in C, C++, and Python

## C and C++

Link the shared library with the `Cephyr::Cephyr` CMake target from
`find_package(Cephyr CONFIG REQUIRED)`, or include `<cephyr/Cephyr.h>` and
link against `libcephyr`. The library and command-line executable run the
same C frontend, lowering, and backend driver. The library never starts
the Cephyr executable.

```c
#include <cephyr/Cephyr.h>
#include <stdio.h>

int main(void)
{
    const char *args[] = {"-S", "-x", "c", "-", "-o", "-"};
    const char source[] = "int answer(void) { return 42; }\n";
    cephyr_request request = {
        .struct_size = sizeof(cephyr_request),
        .argc = sizeof(args) / sizeof(*args),
        .argv = args,
        .stdin_data = source,
        .stdin_size = sizeof(source) - 1,
    };
    cephyr_result *result = NULL;
    int status = cephyr_compile(&request, &result);
    if (result) {
        size_t size;
        const char *data = cephyr_result_output(result, &size);
        fwrite(data, 1, size, stdout);
        data = cephyr_result_diagnostics(result, &size);
        fwrite(data, 1, size, stderr);
        cephyr_result_destroy(result);
    }
    return status;
}
```

In C++17 and later, `<cephyr/Cephyr.hpp>` provides an owning, move-only
`CompileResult`. Its output and diagnostic views borrow the native storage.
Moving a result transfers ownership without invalidating views into that
storage. Destruction or replacement by move assignment invalidates views
into the destroyed storage.

```cpp
#include <cephyr/Cephyr.hpp>

auto result = cephyr::compile({"--emit-mir", "-o", "-", "-"},
                              "int answer(void) { return 42; }\n");
// result.status(), result.output(), result.diagnostics()
```

- Pass driver arguments without `argv[0]`. `-` reads the byte span from the
  request, not the host process's standard input. A zero-size span represents
  empty input. `-E` defaults to captured output, and `--emit-mir -o -` and
  `-S -o -` capture text in the result. `-fsyntax-only` produces diagnostics
  without a compiler artifact. Objects and executables go to files.
- `CEPHYR_OK` and `CEPHYR_COMPILE_ERROR` both return a result. An invalid
  request (`CEPHYR_INVALID_ARGUMENT`) or internal error
  (`CEPHYR_INTERNAL_ERROR`) can return no result. The call overwrites the
  output slot, but doesn't destroy a previous result. Results remain valid
  across later calls. Failure does not roll back files written for earlier
  inputs. Buffers are NUL-terminated; use their reported byte lengths, not
  `strlen`. Use `--diagnostics-format=sarif` for SARIF instead of text.
  Destroy all results before unloading the library.
- `resource_root` selects the directory with `triples/` and `stdlib/`;
  `archon_root` selects the Archon package and `stdspec/` root. Explicit roots
  override the corresponding configured paths. Default installed resources
  are found relative to the loaded library even in an unrelated executable.
  Build-tree binaries fall back to their configured source directories. Keep
  Archon's installed or source layout intact: native IR generation also uses
  the adjacent ASDL and Aurocks scripts and the license file.
  File arguments, `Cephyr.toml`, and environment variables such as
  `CEPHYR_CONFIG` retain their normal driver meanings.
- Cephyr does not change the process working directory, environment, or
  standard streams. The native backend may run the configured host C compiler
  to build generated Archon code. Object and link stages use configured
  assembler, linker, and runtime tools as needed, without invoking a shell.
  Treat settings, roots, SDK hooks, and tools as trusted build inputs;
  embedding is not a sandbox.
- Calls can overlap on different threads. Cephyr serializes access to the
  vendored preprocessor and its own LMDB cache handles, not the entire compiler.
  Use distinct file outputs (or in-memory output) for concurrent requests.
  SDK callbacks run synchronously on the calling thread; recursive calls on
  that thread are rejected.
- A custom SDK assembly callback can add a target selected through a Lua
  target-bank entry. The entry must declare `abi = "LP64"` and match the
  callback's exact target triple to opt into the generic little-endian
  frontend with unsigned plain `char`; it does not receive built-in ISA
  macros. Such targets are not auto-selected, cannot link Cephyr's bundled
  CRT/libc, and leave target ABI implementation to the callback and configured
  tools. The tools must accept the GNU-style directives Cephyr appends for C
  global data.
- Native code generation uses a GIL-enabled CPython main interpreter. If none
  exists on first use, Cephyr initializes one for the life of the process; it
  does not finalize or restart an interpreter. In a host-owned interpreter,
  Cephyr uses that interpreter and
  loads its selected Archon resources under a private Python package name,
  without changing `sys.path` or replacing the host's `archon` package. Do not
  initialize or finalize the interpreter concurrently with compilation.
  Do not call from a subinterpreter. Release the GIL before waiting for
  another thread that needs Cephyr's native backend. Python hosts should
  check `cephyr_python_version()` matches their CPython major/minor before
  calling `cephyr_compile`.

## Python

Use the GIL-enabled CPython main interpreter (3.10 or later, with the same
major/minor as the library). Set `PYTHONPATH` to `<prefix>/<libdir>/cephyr/python`
for a CMake installation, or to `cornerstone/Cephyr/python` in a source tree.

```python
import cephyr

compiler = cephyr.Compiler()
result = compiler.compile(["--emit-mir", "-o", "-", "-"],
                          "int answer(void) { return 42; }\n")
if not result.ok:
    raise RuntimeError(result.diagnostics.decode("utf-8", errors="replace"))
print(result.output.decode("utf-8"))
```

Importing `cephyr` does not load the library. Construction checks the library
ABI and CPython version. It searches an explicit `library=` path first, then
`CEPHYR_LIBRARY`, then its own directory and the CMake installation's libdir,
and finally the system loader path. Supply `library=` or `CEPHYR_LIBRARY` for
a typical source-tree build. The `cephyr.compile(...)` convenience function
accepts the same options and creates a `Compiler` for each call.

As in C, omit `argv[0]` and use `-` to read in-memory input. `str` source is
encoded as UTF-8; `bytes` source is passed with its original length. Paths and
arguments accept `str`, `bytes`, and path-like objects with filesystem encoding.
`Result` owns copies of output and diagnostics as `bytes`, and `Result.sarif()`
parses diagnostics requested in SARIF format. Calls from multiple Python
threads can overlap; Cephyr releases the caller's GIL for native compilation
and acquires it internally for Archon's Python code and SDK callbacks.
Pass `sdk=` with an iterable of trusted Python hooks to either compile function.
See [the Python SDK](SDK.md#python) for supported stages and callback results.
