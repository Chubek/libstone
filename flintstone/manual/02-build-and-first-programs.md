# 2. Build, installation and first programs

## Requirements

Use CMake 3.20 or newer, a C compiler, a C++20 compiler, threads and zlib.
Sandstone can also use system zstd. Blackstone builds the vendored Lua sources.
Tourchstone's XBind tooling uses the vendored QaMRpp headers. Dependencies are
resolved through the repository's `cmake/MLVMDependencies.cmake`; configuration
does not download anything.

From the repository root:

```sh
cmake -S flintstone -B /tmp/opencode/flintstone-build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/opencode/flintstone-build --parallel 2
ctest --test-dir /tmp/opencode/flintstone-build --output-on-failure
```

The source-tree tool is `/tmp/opencode/flintstone-build/flintstone`.
Use a low parallelism setting on memory-constrained machines because XBind's
vendored interpreter headers are substantial.

### Options

| Option | Default | Purpose |
| --- | --- | --- |
| `FLINTSTONE_BUILD_TESTS` | `LIBSTONE_BUILD_TESTS` | C/C++, CLI, backend and relocated-installation tests. |
| `FLINTSTONE_BUILD_TOOLS` | `LIBSTONE_BUILD_TOOLS` | Build/install the textual IL tool. Tests also build it as a helper. |
| `FLINTSTONE_BUILD_BINDING_TESTS` | Flintstone tests | Check the XBind spec and compile generated signature assertions. |
| `LIBSTONE_THIRD_PARTY_DIR` | repository `third_party` | Override the vendored dependency root. |
| `LIBSTONE_LUA_SOURCE_DIR` | vendored Lua directory | Individual Lua override. |
| `XBIND_QAMRPP_ROOT` | vendored `qamrpp` | Individual XBind interpreter override. |

Standalone Flintstone defaults unnecessary sibling test/tool suites off when
their options have not already been set. Explicit user options are retained.
The core, native bridge and binding discovery are not stubbed out by build flags.

### Aggregate build

`LIBSTONE_BUILD_FLINTSTONE=ON` includes Flintstone in the aggregate build. Its
dependencies are still needed if other aggregate component toggles are off.
For a focused root build without the complete Limestone dependency stack:

```sh
cmake -S . -B /tmp/opencode/libstone-flintstone \
  -DLIBSTONE_BUILD_TESTS=OFF \
  -DLIBSTONE_BUILD_LIMESTONE=OFF \
  -DFLINTSTONE_BUILD_TESTS=ON
cmake --build /tmp/opencode/libstone-flintstone --target flintstone-cli
```

This is not a promise that every unrelated component's tests run with only this
target built. The complete Limestone backend has additional documented vendor
API requirements; see `../../README.md` and Limestone's manual.

## First program: an affine expression

`../examples/affine.flint` contains (defaults for target layout and
`arith.overflow` are shown explicitly in the file):

```text
module affine level high {
  target "vm" { pointer_bits = 64; index_bits = 64; endian = "little"; }
  types { T = i32; }
  dialect arith { overflow = "modular"; }
  dialect math { factor = 3; }
  func @affine(%x:param<T>) -> param<T> {
    ^entry:
      %scaled:param<T> = math.scale(%x);
      %answer:param<T> = arith.add(%scaled, 12:param<T>);
      func.return(%answer);
  }
}
```

Run from the repository root:

```sh
/tmp/opencode/flintstone-build/flintstone \
  --run affine --arg 10 flintstone/examples/affine.flint
# 42
```

Inspect verified conversion:

```sh
/tmp/opencode/flintstone-build/flintstone \
  --lower core flintstone/examples/affine.flint
/tmp/opencode/flintstone-build/flintstone \
  --lower machine flintstone/examples/affine.flint
```

Core conversion specializes `T`, replaces `math.scale` with multiplication by
the dialect's factor, and preserves the function signature/results. Machine
conversion uses the VM dialect and requires target `vm`.

## First loop

`../examples/sum.flint` uses predecessor-labelled phis and explicit branches:

```sh
/tmp/opencode/flintstone-build/flintstone \
  --run sum --arg 10 flintstone/examples/sum.flint
# 45
```

Use `--budget 1000` to limit executed bytecode instructions. Budget exhaustion
is a diagnostic, not a returned partial value.

## CLI reference

```sh
/tmp/opencode/flintstone-build/flintstone --help
/tmp/opencode/flintstone-build/flintstone --version
/tmp/opencode/flintstone-build/flintstone --bindings
/tmp/opencode/flintstone-build/flintstone --check flintstone/examples/sum.flint
/tmp/opencode/flintstone-build/flintstone --lower core flintstone/examples/affine.flint
/tmp/opencode/flintstone-build/flintstone --dce flintstone/examples/sum.flint
/tmp/opencode/flintstone-build/flintstone --kernels blackstone/kernels --kernel add_zero flintstone/examples/sum.flint
```

`--kernels DIR --kernel NAME` is repeatable. `--kernel` requires `--kernels DIR`;
`--arg` requires `--run`. Input is one `FILE` or `-` (stdin, the default);
only one input file is accepted and input is capped at 16 MiB. Exit code `2`
means a CLI usage error; `1` means a Flintstone diagnostic or runtime failure.
`i1` CLI arguments accept `0`, `1`, `true` and `false` (the C API accepts only
numeric `0`/`1`).

## Bytecode output

```sh
/tmp/opencode/flintstone-build/flintstone \
  --emit /tmp/opencode/affine.fsvm --backend vm \
  flintstone/examples/affine.flint
```

`--backend vm-stack` emits the alternate Firestone stack model. These are FSVM
files, not native object files. `--emit` refuses to write raw bytes when the
artifact carries relocations, instead of silently discarding them. See
[execution](06-execution.md) for the runtime intrinsics required when consuming
them outside `flintstone::execute`.

## More examples

```sh
/tmp/opencode/flintstone-build/flintstone \
  --scalarize --run dot --arg 6 flintstone/examples/vector.flint
# 36
/tmp/opencode/flintstone-build/flintstone \
  --scalarize --run swap_add --arg 20 --arg 22 flintstone/examples/tuple.flint
# 42
/tmp/opencode/flintstone-build/flintstone \
  --run guarded --arg 0 flintstone/examples/trap.flint
# 42
/tmp/opencode/flintstone-build/flintstone \
  --target x86-64-tiny --backend x86-64-tiny \
  --emit /tmp/opencode/tiny.bin flintstone/examples/sum.flint
# fails: tiny integer targets support single-block functions
```

Vector and tuple programs need `--scalarize` first; neither `lower()` nor
`execute()` invents aggregate elimination silently. `--list-dialects` prints all
ten registered dialects; `--list-targets` prints every backend, tiny native
target and shipped Sandstone architecture.

## Installation

```sh
cmake --install /tmp/opencode/flintstone-build --prefix /path/to/prefix
```

The install provides the shared C ABI, public Flintstone headers, the
`Flintstone.xbind` spec, the CMake package, manual and examples. Sibling Stone
libraries (Firestone, Blackstone, Sandstone, XBind) appear in the prefix because
the build installs all subdirectory targets, not because Flintstone's own
`install()` rules re-install them. `Flintstone::native` additionally exposes the
dedicated native-integration header and narrow Limestone representation headers;
its Sandstone library is resolved from the same prefix via `find_library` (see
`cmake/FlintstoneConfig.cmake.in`) rather than from an exported Sandstone
package. `sandstone.hpp` itself is not installed by Flintstone. A consumer uses:

```cmake
find_package(Flintstone 0.1 CONFIG REQUIRED)
target_link_libraries(my_compiler PRIVATE Flintstone::flintstone)
```

The package does not require source-tree paths. The installation test moves the
prefix before building and executing a C/C++ consumer.

Next: [The language](03-language.md).
