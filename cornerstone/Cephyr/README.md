# Cephyr

See [FactIR](docs/FactIR.md) for the verified MIR rewrite, RV64 scheduler hints,
and the CLI/TOML controls.

Cephyr is a Unix-style C compiler driver for Archon. It currently implements a
subset of ISO C11 for **AMD64 / SysV LP64**, **AArch64 / AAPCS64 LP64**, and
**RV64IM / LP64** on Linux. It is not yet a conforming hosted or freestanding
C11 implementation.
See [the libc inventory](stdlib/README.md) and
[the target bank](triples/README.md) for the runtime and target boundaries, and
[ISO C11 implementation status](C11.md) for language and environment details.

## Build and use

Building the compiler requires CMake 3.20+, a C/C++20 toolchain, Python 3.10+
(Archon and build-time generators), and Perl (QML frontend generation).
Packaging bundled Linux libc additionally needs `ar` and a GNU target compiler
for each of `x86_64-linux-gnu`, `aarch64-linux-gnu`, and `riscv64-linux-gnu`.
Set `CEPHYR_PACKAGE_CC_AMD64`, `CEPHYR_PACKAGE_CC_AArch64`, or
`CEPHYR_PACKAGE_CC_RISCV64IM` to an explicit target compiler, or supply trusted
`CEPHYR_PREBUILT_RUNTIME_ROOT` with `ARCH/{crt0.o,libc.a}` for all three.
Cephyr's uncompressed SymAS banks are generated with the in-tree Lua and
Sandstone assembler-record compiler. No LLVM is required. The default target
is `x86_64-linux-gnu`.

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
through `<cephyr/CephyrSDK.h>`, including handlers for `#pragma cephyr NAME`.
The [Lua extension guide](extension/README.md) describes how to use
`--lua-extension` and the sandboxed `lcephyr` SDK.
See [the Moonstone integration](MOONSTONE.md) for optional compiler tooling
and its trust and verification boundaries.

### Python bindings

The `cephyr` package uses `ctypes` and requires the CPython main interpreter,
built with the GIL. There is no embedded CPython version dependency in the
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

[Configure extensions](extension/CONFIGURATION.md) to load sandboxed Lua
scripts or trusted native SDK plugins from explicit paths. Native plugins
also require `--allow-native-plugins`; the configuration cannot grant it.

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
backend. CMake generates three reviewed Gated/Archon backend modules for the
compiler host and one CRT object and libc archive per Linux target. A native
compile checks the selected specification and imported source digests, then loads
that module. `-v` shows the module and effective tool argv. Artifacts are found
relative to libcephyr after installation; `resource_root`/`stdlib_root` can
select headers, but do not redirect executable package loading. Missing files
or mismatched specifications are errors. Custom targets may supply an exact
SDK backend; new built-in representations or ABIs require rebuilding Cephyr.

No C/C++ compiler driver, archiver, `llvm-mc` or LLD executable is needed on
`PATH` for the bundled static targets, even on the first compile. The package
needs the packaged Archon specification files and a writable temporary directory for
stages which use named intermediates. `--cache`/`--no-cache` and the old cache
path are accepted for compatibility. They do not affect backend selection.
Old `[tools].host_cc`, `runtime_cc`, `archiver`, `[flags].compile` and `runtime`
settings should be removed.

Native qobjld supports bundled static Linux links and AMD64 shared modules.
For AArch64 or RV64 shared objects, explicitly select a capable raw linker
(e.g. `[tools]` followed by `linker = "aarch64-linux-gnu-ld"`). For RV64 external objects,
use `-march=rv64im -mabi=lp64 -mno-relax`; inputs requiring linker relaxation
need an explicit compatible linker. `--stdlib=system` requests an explicitly
configured system compiler-driver linker and matching sysroot/libraries.
Neither a failed assembly nor a failed native link triggers a fallback.

`--emit-mir` is a debugging view; data which cannot be represented by native
MachineIR's word-only globals is emitted separately in `-S` output.

### Blackstone MIR optimization

Blackstone folds block-local integer MIR expressions and simplifies identities
such as `x * 1` without removing operand producers. It runs by default on C
and preprocessed C inputs, after QML and SDK
rewrites and before FactIR. `--emit-mir` shows the result; assembly, object,
and link stages use the same optimized MIR. Preprocessing and syntax checks
skip the pass. The CLI and embedding APIs use the same pipeline.

To disable Blackstone, add the following to `Cephyr.toml`:

```toml
[blackstone]
enabled = false
```

This switch is independent of FactIR and QML.

### QML optimization profiles

QML rewrites Cephyr's typed Archon MachineIR after lowering. No QML pass runs
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
The separate `integer_reduce` pass lowers unsigned remainder by a proven
nonzero power of two. Local known-bit analysis also removes redundant AND
and OR operands and folds bitwise AND to zero when the operands cannot share
a set bit.
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
or link-only inputs; those stages do not load rule files. Built-in rules are precompiled; installed Cephyr uses
`python3` (3.10+) on `PATH` only when compiling user-supplied fragments. Optional
Prolific profiles report `qml_configure` and `qml_rewrite`, as well as
`qml_verify` when the pass changes MIR or a custom backend needs verification.

Treat rules and profiles as **trusted build inputs**. Cephyr structurally checks
the rewritten MIR before it can reach SDK rewrite hooks, MIR output, or a
backend. That check does not prove equivalence or preserve program behavior
automatically.

See the [QML rule format](qml/README.md) and the [Prolific benchmark](benchmark/README.md)
for rule authoring and reproducible measurements.

## Testing with ArchVirt

Run selected freestanding C11 fixtures and the QML differential programs on
ArchVirt's AMD64, AArch64, and RV64IM software machines:

```sh
python3 -B cornerstone/Cephyr/tests/test_cephyr_archvirt.py /path/to/cephyr \
  --archvirt /path/to/archvirt
```

Use `--machine` to select a profile, or set CMake's
`CEPHYR_ARCHVIRT_EXECUTABLE` to register `cephyr.archvirt` with CTest.
The runner needs LLVM `ld.lld` and uses the DomIOS boot ABI, not Linux
system calls. It complements rather than replaces hosted libc testing.
The x86 software profile omits the larger record and CFG fixtures because
they emit instructions (`CQO`/`IDIV`) that it does not support.

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
  `long double` storage, scalar arithmetic, comparisons, conversions, and
  by-value parameters and returns for prototyped functions. RV64IM supports
  software floating point and fixed-prototype
  passing and returning of supported integer/pointer records. Bit-fields,
  including volatile accesses, are supported outside by-value ABI boundaries. Atomics and VLAs
  remain unsupported. See [C11 status](C11.md) for details.
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
