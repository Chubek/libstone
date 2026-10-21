# 2. Building and installing

## Prerequisites

Use CMake **3.20 or newer** and a working C/C++ toolchain. The full tree requires
C++20; Firestone alone needs C++17. A normal build uses system threads.

Additional requirements depend on the selected facilities:

| Facility | Additional prerequisites |
| --- | --- |
| Blackstone; Flintstone | Vendored Lua sources; Flintstone also needs zlib and QaMRpp headers |
| Sandstone core | zlib; zstd is detected optionally |
| Sandstone spec compiler | Boost.JSON |
| Sandstone original `.dsymspec` evaluation | A Lua interpreter and an external `dsl.lua` runtime |
| SandSLAB / C++ small-ISA loader | MetaTk's `DSLtk/DSLtk.hpp` |
| Full Limestone | Compatible vendor snapshots, Perl for parser/AST generation; LMDB and libffi unless disabled |
| Python tests / XBind tests | Python; XBind tests require Python 3.8+ |
| Limestone Python bindings | SWIG 4 and Python development-module support |
| D MachineIR tests | `dub` and a D compiler; exchange tests additionally require `dmd` or `ldc2` |

CMake does **not** download dependencies. Populate `third_party/` or supply
explicit paths before configuring. Read [dependency configuration](03-configuration.md)
before attempting a full Limestone build: upstream snapshots may pass file checks
but lack required APIs.

## Minimal Firestone build

This is the smallest starting point and needs no vendored libraries:

```sh
cmake -S firestone -B build-firestone -DCMAKE_BUILD_TYPE=Debug
cmake --build build-firestone --parallel 2
ctest --test-dir build-firestone --output-on-failure
```

Firestone is header-only, but its default build compiles tests and independent
public-header checks. For an installation-only configuration, use a fresh
directory with `-DBUILD_TESTING=OFF`.

## Standalone component builds

Run these from the repository root, with dependencies already populated:

```sh
cmake -S blackstone -B build-blackstone -DCMAKE_BUILD_TYPE=Debug
cmake --build build-blackstone --parallel 2
ctest --test-dir build-blackstone --output-on-failure

cmake -S flintstone -B build-flintstone -DCMAKE_BUILD_TYPE=Debug
cmake --build build-flintstone --parallel 2
ctest --test-dir build-flintstone --output-on-failure

cmake -S sandstone -B build-sandstone -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sandstone --parallel 2
ctest --test-dir build-sandstone --output-on-failure
```

Standalone means independently configured, not dependency-free. Flintstone adds
Firestone, Blackstone, Sandstone core, and XBind from sibling directories. When
the full Limestone targets are absent, it compiles a narrow set of Limestone
scheduling/allocation sources instead of requiring the complete solver/parser
stack. It defaults unnecessary sibling tests and tools off if their options
have not already been set.

For XBind without the retired native-loader dependency:

```sh
cmake -S tourchstone -B build-tourchstone \
  -DTOURCHSTONE_BUILD_DYNLOAD=OFF \
  -DTOURCHSTONE_BUILD_DYNINVOKE=OFF
cmake --build build-tourchstone --parallel 2
ctest --test-dir build-tourchstone --output-on-failure
```

Alternatively configure `tourchstone/xbind` directly; its executable will be
`build-xbind/xbind` rather than `build-tourchstone/xbind/xbind`:

```sh
cmake -S tourchstone/xbind -B build-xbind -DXBIND_BUILD_TESTS=ON
cmake --build build-xbind --parallel 2
ctest --test-dir build-xbind --output-on-failure
```

## Aggregate build

Once all required dependency revisions are available:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Blackstone, Firestone, Limestone, Sandstone, Tourchstone, and Flintstone are
selected by default. Yellowstone is off. The optional Gatestone switch is off,
and this checkout has no active `gatestone/` source directory.

Root switches select which components are explicitly added. They do not forbid
another selected component from building a required sibling. For example,
disabling `LIBSTONE_BUILD_BLACKSTONE` does not remove Flintstone's Blackstone
dependency.

For a root build restricted to Firestone:

```sh
cmake -S . -B build-foundations \
  -DLIBSTONE_BUILD_BLACKSTONE=OFF \
  -DLIBSTONE_BUILD_LIMESTONE=OFF \
  -DLIBSTONE_BUILD_SANDSTONE=OFF \
  -DLIBSTONE_BUILD_TOURCHSTONE=OFF \
  -DLIBSTONE_BUILD_FLINTSTONE=OFF
cmake --build build-foundations --parallel 2
ctest --test-dir build-foundations --output-on-failure
```

Use standalone Flintstone if you want its IL tools without the complete
Limestone backend. For the complete backend, configure `limestone` directly
with compatible snapshots as described in [Chapter 3](03-configuration.md#limestone-revision-compatibility).

The committed `CMakePresets.json` is empty; the commands in this manual do not
rely on a named preset.

## Installation

Install from a successfully built binary directory:

```sh
cmake --install build-firestone --prefix /tmp/opencode/libstone-prefix
```

Substitute your aggregate or component binary directory as appropriate.
Installation includes enabled subdirectory install rules, so standalone
Flintstone also installs its built sibling dependencies. Use a separate prefix
for incompatible configurations.

GNU installation directories are used (`bin`, `include`, `lib` or `lib64`, and
`share`); override them through the usual `CMAKE_INSTALL_*DIR` variables.
Consumers should use exported targets where available rather than hard-coding
archive paths. There is no single aggregate `Libstone::libstone` target or
repository-wide `find_package(Libstone)` package.

The repository-level manual is read from `manual/`; no root install rule copies
it. Individual components have their own documentation installation policies.

Next: [Configuration and dependencies](03-configuration.md).
