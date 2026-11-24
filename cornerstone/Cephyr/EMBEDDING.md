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
  `-S -o -` capture text in the result. `-c -o -` captures a relocatable
  object and `-shared -o -` captures a shared object. Executables still go
  to files, and the CLI does not support writing binary artifacts to stdout.
  `-fsyntax-only` produces diagnostics without a compiler artifact.
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
  Build-tree binaries use configured source and package directories. Installed
  libraries use their own packaged backend modules and target runtimes; preserve
  the `share/cephyr/native-packages` and Archon resource trees when relocating.
  File arguments, `Cephyr.toml`, and environment variables such as
  `CEPHYR_CONFIG` retain their normal driver meanings.
- Cephyr does not change the process working directory, environment, or
  standard streams. The native backend loads a prebuilt module. Object and link
  stages invoke the packaged SandAS and qobjld, or configured replacements,
  directly. Bundled links use prebuilt CRT/libc without a target compiler.
  Treat settings, roots, SDK hooks, and tools as trusted build inputs;
  embedding is not a sandbox. [Lua extensions](extension/README.md) are
  separately sandboxed and may be supplied as script bytes and an explicit
  environment via `cephyr_request.lua` or `cephyr::CompileOptions::lua`.
  Context-based and Python callers can use the driver options.
- Calls can overlap on different threads. Cephyr serializes access to the
  vendored preprocessor. Each translation unit owns its FactIR result.
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
- The native library does not initialize or link a Python interpreter.
  `cephyr_python_version()` returns zero. The compiler checks the selected
  architecture's source manifest and loads its packaged backend directly.

To load a Lua extension from memory, attach a `cephyr_lua_options` to the
request before calling `cephyr_compile`:

```c
const char script[] = "assert(require('lcephyr').env.get('BUILD_MODE') == 'checked')";
const cephyr_lua_script scripts[] = {{script, sizeof(script) - 1, "build.lua"}};
const cephyr_lua_env_entry environment[] = {{"BUILD_MODE", "checked"}};
const cephyr_lua_options lua = {sizeof(lua), scripts, 1, environment, 1};
request.lua = &lua;
```

The strings and descriptors are borrowed until the call returns. The script
name is a diagnostic label; the compiler does not open it. Every script in
the request can read all explicitly supplied values, and none of the process
environment is inherited.

## Reusing a context and loading trusted code (C API)

Create a `cephyr_context` to keep a copy of driver arguments and resource
roots across calls. Include an input operand of `-` to compile the source
span passed to `cephyr_context_compile`. Each call is a separate driver
invocation: C declarations, macro definitions, object files, and SDK hooks
do not accumulate. Supply SDK hooks per call; they are borrowed only for that
call. Results outlive the context. A context can be shared among compiling
threads, but don't destroy it while any call is using it.

For a multi-file module, call `cephyr_context_add_translation_unit` once per
C source span, then `cephyr_context_link(context, sdk, &artifact)`. Keep a
single C `-` operand alongside `-shared -o -`: the units replace that operand
in append order. A unit's optional logical filename is used for diagnostics
and preprocessing. Each unit is compiled independently, then the objects are
linked together once. You can append another unit and retry without losing
earlier units. `cephyr_context_reset` clears the collection and registered
symbols without changing the driver arguments. Concurrent links use snapshots;
results and loaded modules do not borrow the context.

On AMD64 Linux with qobjld or direct LLD, `cephyr_context_register_host_symbol` can bind
an external C function or data declaration in those links. For example, register
`"counter"` as `CEPHYR_HOST_DATA` with `&counter`, and declare it as
`extern int counter;` in the source. This refers to the actual host storage; a
function registration instead uses an ABI-preserving trampoline and is not
guaranteed to compare equal to the original function pointer. Use a matching
signature or data type and keep the target address valid through all module
calls and destructors. Only subsequent links see registration changes.
Registered names are reserved for imports. These bindings are not globally
visible, and artifacts containing them are only meaningful in the linking
process—do not save or distribute their output bytes. Unregistered references
follow ordinary Linux loader rules; a missing strong symbol fails eager load.

Use `cephyr_module_load` on a successful `-shared -o -` result to run trusted
code for the host ABI. This example uses a dependency-free module:

```c
#include <cephyr/Cephyr.h>
#include <stdio.h>

int main(void)
{
    const char *const args[] = {
        "--target=auto", "-shared", "--stdlib=none", "-nostdlib", "-o", "-", "-"
    };
    const cephyr_context_options options = {
        .struct_size = sizeof(options), .argc = sizeof(args) / sizeof(*args), .argv = args
    };
    const char source[] = "int answer(void) { return 42; }\n";
    cephyr_context *context = NULL;
    cephyr_result *artifact = NULL, *details = NULL;
    cephyr_module *module = NULL;
    void *symbol = NULL;

    int status = cephyr_context_create(&options, &context);
    if (status == CEPHYR_OK)
        status = cephyr_context_compile(context, source, sizeof(source) - 1, NULL, &artifact);
    if (status == CEPHYR_OK)
        status = cephyr_module_load(artifact, &module, &details);
    if (status == CEPHYR_OK)
        status = cephyr_module_symbol(module, "answer", &symbol);

    const int ok = status == CEPHYR_OK && symbol;
    if (ok)
        printf("%d\n", ((int (*)(void))symbol)());  /* POSIX dlsym conversion */
    else if (details || artifact)
        fputs(cephyr_result_diagnostics(details ? details : artifact, NULL), stderr);
    cephyr_module_destroy(module);
    cephyr_result_destroy(details);
    cephyr_result_destroy(artifact);
    cephyr_context_destroy(context);
    return !ok;
}
```

- Native capture returns a length-delimited byte span (up to 128 MiB),
  including embedded NUL bytes. With the default SandAS/qobjld path,
  embedding assembles through memory files and can capture the shared link
  into an anonymous memory file. This is not a diskless toolchain:
  external QML bundles, configured tool or
  pass-through cases, and atomic filesystem publication can still use
  named files. Memory files also require access to `/proc/self/fd`.
- Loading uses the Linux dynamic loader with eager relocation and local
  visibility (`RTLD_NOW | RTLD_LOCAL`). It normally loads from an anonymous
  memory file; without memfd/proc support it uses a private temporary
  directory and unlinks the artifact after loading. The backing filesystem
  must allow executable mappings. Dependencies follow ordinary loader rules;
  do not rely on `$ORIGIN` referring to the app or source directory. There is
  no relocatable-object loader or change to the process's global symbol scope.
- Load only code you trust to run with your application's privileges.
  Constructors may execute during load, and destructors may execute when the
  loader reference is released. Keep `libcephyr` loaded while any module,
  context, or result is live. A module does not borrow the context or result;
  symbol addresses borrow the module. Stop using them and finish outstanding
  calls before destroying it. Use the actual ABI signature for a function.
  Check the lookup status: a valid ELF symbol can have a null address.
- C++ and Python's one-shot `compile` wrappers can also capture binary
  artifacts. The context and loader entry points are currently C APIs.

## Python

Use the GIL-enabled CPython main interpreter (3.10 or later). Set `PYTHONPATH` to `<prefix>/<libdir>/cephyr/python`
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
ABI (and any interpreter ABI reported by older libraries). It searches an explicit `library=` path first, then
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
and Python SDK callbacks acquire it when entered.
Pass `sdk=` with an iterable of trusted Python hooks to either compile function.
See [the Python SDK](SDK.md#python) for supported stages and callback results.
## Parse-only C11 and AST input

Call `cephyr_parse_c11(request, CEPHYR_AST_JSON, &result)` or
`cephyr::parse_c11({"-"}, CEPHYR_AST_JSON, source)` to get a syntax document
in the result buffer. `CEPHYR_AST_YAML` and `CEPHYR_AST_SEXP` select other
formats. To compile from a result buffer, use the ordinary compile API with
`-x c11-ast-json -` (or `c11-ast-yaml` / `c11-ast-sexp`) and pass the document
as stdin data. Contexts can use the same flags via `cephyr_context_compile`.
The Python SDK exposes `cephyr.parse_c11` and `Compiler.parse_c11`.
See [the format and CLI examples](docs/C11Syntax.md).

## Resolved C11 declarations

Call `cephyr_emit_interface(&request, &result)` with the same request and
result types as `cephyr_compile`. Omit the stage option from `request.argv`;
the entry point selects `--emit-interface=sexp`. By default the interface
text is returned by `cephyr_result_output`. Check the status, read any
diagnostics with `cephyr_result_diagnostics`, and release the result with
`cephyr_result_destroy` as usual.

```cpp
auto result = cephyr::emit_interface({"--target=x86_64-linux-gnu", "-"},
                                     "typedef unsigned long word;\nextern word count;\n");
```

```python
result = compiler.emit_interface(
    ["--target=x86_64-linux-gnu", "-"],
    "typedef unsigned long word;\nextern word count;\n",
)
if not result.ok:
    raise RuntimeError(result.diagnostics.decode("utf-8", errors="replace"))
print(result.output.decode("utf-8"))
```

The Python convenience function `cephyr.emit_interface(...)` accepts the
same arguments and an optional `library=` override.

This operation resolves declarations for the selected C target but does not
lower function bodies or emit machine code. For the output format, scope, and
limits, see [C11 declaration interfaces](docs/C11Interface.md).
