# 7. Testing and troubleshooting

## Normal validation

Configure the selected component or aggregate tree, build the full selected
test configuration, and run CTest:

```sh
cmake -S firestone -B build-firestone -DCMAKE_BUILD_TYPE=Debug
cmake --build build-firestone --parallel 2
ctest --test-dir build-firestone --output-on-failure
```

List registered tests without executing them:

```sh
ctest --test-dir build-firestone -N
```

In an aggregate binary directory, select a family by name:

```sh
ctest --test-dir build -R '^firestone\.' --output-on-failure
ctest --test-dir build -R '^blackstone\.' --output-on-failure
ctest --test-dir build -R '^flintstone\.' --output-on-failure
ctest --test-dir build -R '^xbind\.' --output-on-failure
```

Sandstone and Limestone also use test names without those dotted prefixes, so
use `ctest -N` instead of assuming a universal naming scheme. Building only one
CLI target does not build every test executable registered by its configuration.

Use `Debug` for development validation; several suites rely on assertions.
Installed-consumer tests exercise a different boundary from source-tree tests:
they catch missing headers, exported dependencies, data paths, and relocation
problems. Run them when changing installation rules or public interfaces.

## What to test

| Change | Minimum useful evidence |
| --- | --- |
| New representation or parser | Valid examples, malformed input, deterministic output, verifier rejection |
| Rewrite or lowering | Positive/negative cases, type/effect boundaries, valid output, unchanged input on failure |
| Scheduling/allocation | Semantic dependencies, fixed operands, spills/transfers, resource legality |
| Instruction encoding | Known bytes, range failures, round trips where supported, relocation cases |
| Native backend | Host/ABI gating and executed differential tests, not only assembly text |
| Public API or install layout | C/C++ consumer, handle lifetimes/errors, moved-prefix test |
| Generated binding | Signature compilation plus actual module loading/calls |

Do not report an optional test as passed merely because it was not registered.
D MachineIR needs `dub` and a compiler; the C++/D exchange needs `dmd` or `ldc2`.
Run the D package directly when equipped:

```sh
(cd limestone/metacode/machine-ir && dub test)
```

Sandstone's Python tests, XBind generated-wrapper tests, native host-specific
checks, and manifest checks also have prerequisites. Record skipped coverage.

## Sanitizers

A separate Clang configuration can enable address/undefined-behavior checks:

```sh
cmake -S firestone -B build-firestone-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-firestone-sanitize --parallel 2
UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-firestone-sanitize --output-on-failure
```

For mixed C/C++ and shared-library builds, set corresponding
`CMAKE_C_FLAGS` and `CMAKE_SHARED_LINKER_FLAGS` as well. Some ptrace-based
environments prevent LeakSanitizer startup; `ASAN_OPTIONS=detect_leaks=0` is a
documented workaround, but then leak checking is **not** performed.

## Troubleshooting

### Configure reports a missing vendored file

Use the exact source directory named in the diagnostic. Confirm that it contains
the required source files, not just installed headers. Populate a compatible
snapshot or set its `LIBSTONE_*_SOURCE_DIR`. No automatic download is attempted.
Check `cmake -LAH -N build` for stale per-dependency paths before changing the
dependency root again.

### Limestone configures but compilation fails in Equinox-NG or EkippX

File presence is not API compatibility. Errors involving `set_observers`,
`set_interrupt`, `add_node`, or `MappedExpansion` indicate the revision boundary
described in [Chapter 3](03-configuration.md#limestone-revision-compatibility).
Select compatible Equinox-NG and MetaTk snapshots in a new build directory. Do
not suppress the calls or substitute a different algorithm to hide the mismatch.

### Tourchstone says it needs active Yellowstone qBFD

Keep `TOURCHSTONE_BUILD_DYNLOAD=OFF` and
`TOURCHSTONE_BUILD_DYNINVOKE=OFF` for the active XBind-only checkout. Turning on
`LIBSTONE_BUILD_YELLOWSTONE` does not restore `.decomissioned/yellowstone`;
the root requires an active source directory. Retired code is not automatically
revived by CMake.

### Sandstone cannot find Boost.JSON, Lua, or `dsl.lua`

Boost.JSON is needed by the spec compiler, not by the core databank library.
Disable `SANDSTONE_BUILD_COMPILER` if you only need the core. Lua and the external
DSL runtime are needed to evaluate original `.dsymspec` sources; JSON specs can
be compiled without them. Use `--dsl-dir` for the Python driver or configure
`SANDSTONE_DSL_DIRECTORY` for the relevant compiler/tests. Supply the driver's
`--compiler` path when the build is outside its discovery path.

### A VM reports an unbound Flintstone native intrinsic

When loading emitted FSVM directly, call `flintstone::bind_vm_runtime` on the
execution environment. Ordinary Firestone execution does not invent
Flintstone-specific native declarations. Also bind application externals with
matching signatures. Do not remove width guards or modular arithmetic intrinsics.

### A tool exists despite `LIBSTONE_BUILD_TOOLS=OFF`

Tests can require CLI helpers, XBind has separate executable rules, and cached
component options may still be on. Inspect component test/tool switches.
Shared switches seed defaults; they are not a blanket prohibition on executables.

### CTest finds no tests or cannot locate a test executable

Check `BUILD_TESTING`, `LIBSTONE_BUILD_TESTS`, and the component's cached
`*_BUILD_TESTS`. Use the actual binary directory, not the source directory, and
build the complete test configuration first. For multi-config generators, pass
the same configuration to both the build and CTest.

### The compiler is killed or the build exhausts memory

Reduce parallelism to `--parallel 1` or `2`, particularly for the substantial
QaMRpp/XBind interpreter headers. A killed compiler is not by itself evidence
of a source-level error.

### An installed consumer cannot find a package or data files

Pass the installation prefix through `CMAKE_PREFIX_PATH`, not the source root.
Ensure all required components were installed into a compatible search path;
Blackstone's installed package resolves Firestone. Sandstone and XBind do not
currently provide standalone package configs. Use `Blackstone_KERNEL_DIR` for
installed kernels and `XBIND_DATA_DIR=/prefix/share/xbind` for relocated XBind
type-map data. Do not assume the build directory's data paths remain deployed.

### A rewrite, emitter, or native target rejects an input

First check the documented support boundary. Modular semantics, narrow integer
domains, scalarization, target CFG restrictions, and relocation constraints are
deliberate checks. An architecture name or XBind target entry does not imply
that all operations, ABIs, or wrappers are implemented.

## Recording a reproducible failure

Include the configure command, compiler/CMake versions, selected component,
relevant cached options, dependency revisions, first useful diagnostic, and
smallest failing input. For execution problems, also include the target,
arithmetic semantics, host architecture, execution budget, and external bindings.
State which tests actually ran and which prerequisites were unavailable.

Next: [Reference and further reading](08-reference.md).
