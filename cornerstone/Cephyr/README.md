# Cephyr

Cephyr is a Unix-style C compiler driver for Archon. It currently implements a
subset of ISO C11 for **AMD64 / SysV LP64**, **AArch64 / AAPCS64 LP64**, and
**RV64IM / LP64** on Linux. It is not yet a conforming hosted or freestanding
C11 implementation.
See [the libc inventory](stdlib/README.md) and
[the target bank](triples/README.md) for the runtime and target boundaries, and
[ISO C11 implementation status](C11.md) for language and environment details.

## Build and use

Requires CMake 3.20+, a C++20 compiler, CPython 3.10+ with embedding development
files, Perl (to generate the QML frontend), and LMDB development files.
Lua, Kaguya, ucpp, tomlc99, lmdbxx, and
cparser-iso11 are taken from `third_party/`; the build does not download them.
The bundled target bank uses `clang`, `ld.lld`, and `llvm-ar` to assemble and
link ELF output. The default target is `x86_64-linux-gnu`.

```sh
cmake -S cornerstone/Cephyr -B build-cephyr
cmake --build build-cephyr
ctest --test-dir build-cephyr --output-on-failure

build-cephyr/cephyr -E -Iinclude example.c
build-cephyr/cephyr -fsyntax-only example.c
build-cephyr/cephyr --emit-mir example.c -o example.mir
build-cephyr/cephyr -S example.c -o example.s
build-cephyr/cephyr -c example.c -o example.o
build-cephyr/cephyr main.c example.o -o example
```

Example `main.c`:

```c
#include <stdio.h>

int main(void) {
    puts("Hello from Cephyr");
    return 0;
}
```

Run the default ELF on x86-64 Linux. For AArch64, compile with
`--target=aarch64-linux-gnu` and run on AArch64 Linux or with `qemu-aarch64`.
For RISC-V, use `--target=riscv64-linux-gnu` and run on RV64 Linux or with
`qemu-riscv64`. To pass an emulator path to CTest, configure
`CEPHYR_QEMU_AARCH64_EXECUTABLE` or `CEPHYR_QEMU_EXECUTABLE` (RV64).
Cephyr can also be enabled
from LibStone's root build with `-DLIBSTONE_BUILD_CEPHYR=ON`. Use
`cmake --install build-cephyr --prefix PREFIX` for an installed compiler,
target definitions, Archon resources, and libc sources.

## Embedding and extensions

The installed shared library, `libcephyr`, exposes a versioned C ABI through
`<cephyr/Cephyr.h>` and a C++ convenience wrapper in `<cephyr/Cephyr.hpp>`.
CMake applications can use `find_package(Cephyr CONFIG REQUIRED)` and link
`Cephyr::Cephyr`. The Python `cephyr` package calls the same library in-process;
it does not need the `cephyr` executable. Set `PYTHONPATH` to
`<prefix>/<libdir>/cephyr/python` for a CMake installation.

See the [embedding guide](EMBEDDING.md) for C and C++ examples and lifecycle
requirements. The [extension SDK](SDK.md) provides opt-in, trusted callbacks
through `<cephyr/CephyrSDK.h>` without exposing parser internals as public ABI.
See [the Moonstone integration](MOONSTONE.md) for optional compiler tooling
and its trust and verification boundaries.

### Python bindings

The `cephyr` package uses `ctypes` and requires the CPython main interpreter,
built with the GIL, and the same CPython major/minor version used to build the
native library. Calls can overlap and the wrapper releases the GIL while the
native compiler runs. The compiler can still run its configured build tools
when needed.

In a CMake install the package finds the shared library relative to itself.
From the source tree, use `PYTHONPATH=cornerstone/Cephyr/python` and set
`CEPHYR_LIBRARY` to the path of the built shared library. You can also pass
`library=...` to override discovery. Importing the module does not load the
library.

```python
import cephyr

compiler = cephyr.Compiler()
result = compiler.compile(
    ["-x", "c", "-", "-S", "-o", "-", "--diagnostics-format=sarif"],
    "int add(int a, int b) { return a + b; }\n",
)
if result.ok:
    print(result.output.decode("utf-8"))
else:
    print(result.diagnostics.decode("utf-8"))
```

Pass driver arguments without the program name. In-memory source is read
from `-` and can be UTF-8 `str` or verbatim `bytes`; argv entries and optional
`resource_root` and `archon_root` paths accept strings, filesystem bytes, or
path-like objects. A `Result` contains a `Status` and two byte strings,
`output` and `diagnostics`. `Result.sarif()` parses diagnostics requested
with `--diagnostics-format=sarif`. Use `-o -` for in-memory text output where
the driver supports it. The `cephyr.compile(...)` convenience function accepts
the same compile arguments and an optional `library=...` path. Both accept
`sdk=` for [trusted Python hooks](SDK.md#python).

## Configuration and compilation

Settings are applied in this order: built-in defaults, `./Cephyr.toml` (or
`CEPHYR_CONFIG`, or `--config FILE`), then CLI overrides. The
[sample configuration](Cephyr.toml) documents the strict TOML schema, path
resolution, and tool argv arrays. A triple selects a Lua table; a custom
table can point to an explicit Archon specification and set assembler,
linker, and archiver commands. Lua tables run with memory and instruction
limits and no standard file or process libraries. Configuration, target
definitions, Archon resources, tools, and the local cache are trusted build
inputs.

### Host target selection

With no target setting the default is `x86_64-linux-gnu`, even on a
different host. Use `--target=auto` or `target = "auto"` in TOML to opt in to
detecting the execution host. Cephyr uses the kernel's machine and OS and its
own process ABI, not the target advertised by a configured compiler or assembler.
Auto recognizes Linux/glibc LP64 hosts running AMD64, little-endian AArch64,
or little-endian RV64 with I and M and a soft-float ABI. AArch64 ILP32 and
big-endian AArch64, RISC-V LP64D, and AMD64 x32 are not eligible. Auto reports a
diagnostic instead of silently choosing a cross or VM target.

A concrete CLI triple overrides a TOML triple. It also bypasses a configured
script unless it repeats the TOML triple (or repeats the built-in default when
TOML omits `target`). A non-empty `paths.triple_script` bypasses auto detection;
if no concrete triple is supplied, the script supplies it. For auto builds
Cephyr checks that the effective tools for the requested stage are executable
before compiling; `-E`, `-fsyntax-only` and ordinary `--emit-mir` need no external tools. Tool
discovery does not validate an overridden tool's target flags, linker sysroot,
or library availability.

See the [target bank](triples/README.md#automatic-selection) for how additional
host mappings become eligible.

### Compilation pipeline

ucpp preprocesses C and `.S` inputs. cparser-iso11 parses the C translation
unit, Wetstone evaluates integer constant expressions and analyzes control
flow, and Syren reports source diagnostics in text or SARIF. The compiler
lowers the AST to typed native Archon MachineIR and invokes a generated native
backend. It does **not** delegate user C source to Clang or GCC; a separate
configurable compiler builds Cephyr's packaged libc sources.

On each invocation that needs native C output, the driver regenerates the
backend through Archon's C++ IRGen API unless the architecture, generation
settings, and generator inputs match an LMDB cache entry. The cache stores
generated source artifacts only, never user sources, user object code, or
loadable host libraries. The private library is built for the current
invocation; `-v` reports a hit or regeneration. Use `--no-cache` to bypass
LMDB. Preprocessing, frontend checks, and `--emit-mir` do not build a backend.
`--emit-mir` is a debugging view; data which cannot be represented by native
MachineIR's word-only globals is emitted separately in `-S` output.

### QML optimization profiles

QML rewrites Cephyr's typed Archon MachineIR after lowering. Nothing runs
by default. Enable a built-in pass directly or select a profile:

```sh
cephyr --emit-mir --qml-pass=canonicalize example.c -o -
cephyr --config project/Cephyr.toml --profile=fast -c example.c
```

```toml
[qml]
profile = "fast" # Optional default; --profile overrides it.

[profiles.fast]
passes = ["canonicalize", "strength_reduce", "project_optimizations"]
files = ["rules/project.qpass"]
fuel = 4096
parameters = { project_optimizations = { fuel = 256 } }
```

`passes` lists QML phase names in execution order. The built-in `canonicalize`
pass applies identities such as `x + 0 → x` to native integer MIR.
`strength_reduce` rewrites wrapping multiplication by two to addition.
A rule fragment may supply another phase, for example:

```qml
rewrite project_optimizations eliminate_sub_zero : sub(?x, 0) => ?x;
```

Profile `files` are relative to the configuration file; repeatable `--qml-file`
paths are relative to the working directory. Cephyr compiles all selected files
together once per invocation, in profile-then-CLI file order. Each repeatable
`--qml-pass` appends a phase after the profile passes. Select at least one pass
when passing files or fuel. `--qml-fuel N` overrides profile fuel (default 4096).
The repeatable `--qml-param PHASE.fuel=N` overrides phase-local fuel; only `fuel`
is supported and the named phase must be selected. Fuel settings accept
integers from 1 to 1,000,000. The global limit counts changes across the
translation unit, and a phase-local limit caps changes within that pass.
Unknown profiles, phases and parameter keys are errors.

QML operates on C and preprocessed C inputs for `--emit-mir`, `-S`, `-c`,
and linking from source. QML is not run for `-E`, `-fsyntax-only`, assembler,
or link-only inputs; those stages do not load rule files. QML processing does
not alter the generated Archon backend cache key because that cache stores no
per-input artifacts. Built-in rules are precompiled; installed Cephyr uses
`python3` (3.10+) on `PATH` only when compiling user-supplied fragments. Optional
Prolific profiles report `qml_configure` and `qml_rewrite`, as well as
`qml_verify` when the pass changes MIR or a custom backend needs verification.

Treat rules and profiles as **trusted build inputs**. Cephyr structurally checks
the rewritten MIR before it can reach SDK rewrite hooks, MIR output, or a
backend. That check does not prove equivalence or preserve program behavior
automatically.

See the [QML rule format](qml/README.md) and the [Prolific benchmark](benchmark/README.md)
for rule authoring and reproducible measurements.

## Current limits

- The native C subset covers integer and pointer operations, fixed-signature
  calls, direct external scalar variadic calls, scalar and aggregate storage,
  structure and union member access and copy, flexible array members,
  designated initializers, enums, fundamental alignment specifiers, `offsetof`,
  fixed-size `sizeof` in integer constant expressions, scalar volatile access,
  `_Static_assert`, `__func__`, `_Generic`, loops, `goto`, and `switch` including
  fallthrough.
  AMD64 and AArch64 also support native `float`/`double` arithmetic, conversions,
  comparisons, and fixed-prototype calls. Both also support a scalar subset
  of `<stdarg.h>` and direct floating-point variadic calls. AMD64 supports
  `long double` storage and constant initialization, but not evaluated runtime
  values. RV64IM supports limited software floating point and fixed-prototype
  passing and returning of supported integer/pointer records. Bit-fields,
  atomics, and VLAs remain unsupported. See [C11 status](C11.md) for details.
  `-fsyntax-only` checks the implemented frontend subset, not all of ISO C11.
- The default `stdlib = "cephyr"` builds a self-contained **static** Linux
  executable with Cephyr's CRT and libc. Use `-nostdlib` or `stdlib = "none"`
  with an explicitly supplied runtime for bare-metal programs, or with
  `-shared` for a shared object that does not require the bundled runtime.
- For a system CRT and libc, use `stdlib = "system"` and explicitly configure
  a target compiler-driver linker and its sysroot/flags as needed. Cephyr can
  request dynamic linkage and emit PIC; it does not supply a target dynamic
  loader, system headers, or a dynamic build of its own libc. Headers using
  unsupported compiler extensions may not parse.
- Each compilation obeys the native backend's MachineIR and frame limits.
  GNU driver extensions and later C standards are not emulated; run
  `cephyr --help` for supported flags.
