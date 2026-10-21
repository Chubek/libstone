# 3. Configuration and dependencies

## Shared build switches

The shared definitions are in [LibstoneBuild.cmake](../cmake/LibstoneBuild.cmake).

| Option | Initial default | Effect |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | CTest's conventional testing switch; seeds Libstone's test default |
| `LIBSTONE_BUILD_TESTS` | `BUILD_TESTING` | Default for component test switches |
| `LIBSTONE_BUILD_TOOLS` | `ON` | Default for component tool switches |
| `LIBSTONE_ENABLE_WARNINGS` | `ON` | Enable the shared warning flags |
| `LIBSTONE_BUILD_BLACKSTONE` | `ON` | Add Blackstone in the root build |
| `LIBSTONE_BUILD_FIRESTONE` | `ON` | Add Firestone in the root build |
| `LIBSTONE_BUILD_LIMESTONE` | `ON` | Add the complete Limestone stack |
| `LIBSTONE_BUILD_SANDSTONE` | `ON` | Add Sandstone |
| `LIBSTONE_BUILD_TOURCHSTONE` | `ON` | Add Tourchstone |
| `LIBSTONE_BUILD_FLINTSTONE` | `ON` | Add Flintstone |
| `LIBSTONE_BUILD_YELLOWSTONE` | `OFF` | Requires an active `yellowstone/CMakeLists.txt`, absent in this checkout |
| `LIBSTONE_BUILD_GATESTONE` | `OFF` | Requires an active `gatestone/`, absent in this checkout |

These are **initial defaults**, not forced synchronization. Once a component's
cache option is set, changing `LIBSTONE_BUILD_TESTS` does not reset it. Use a
fresh build directory when switching substantially different configurations.
Legacy `MLVM_BUILD_TESTS`, `MLVM_BUILD_TOOLS`, and `MLVM_ENABLE_WARNINGS` can seed
the corresponding defaults for compatibility.

## Component switches

| Component | Options and behavior |
| --- | --- |
| Firestone | `FIRESTONE_BUILD_TESTS`; `FIRESTONE_ENABLE_OPENMP=OFF` enables OpenMP host compute when requested |
| Blackstone | `BLACKSTONE_BUILD_TESTS`, `BLACKSTONE_BUILD_TOOLS` inherit shared defaults |
| Flintstone | `FLINTSTONE_BUILD_TESTS`, `FLINTSTONE_BUILD_TOOLS`; `FLINTSTONE_BUILD_BINDING_TESTS` defaults to its test setting |
| Limestone | `LIMESTONE_BUILD_TESTS`, `LIMESTONE_BUILD_TOOLS`; `LIMESTONE_ENABLE_PERSISTENT_CACHE=ON`, `LIMESTONE_ENABLE_NATIVE_FFI=ON`, `LIMESTONE_BUILD_PYTHON_BINDINGS=OFF` |
| Sandstone | `SANDSTONE_BUILD_TESTS`, `SANDSTONE_BUILD_TOOLS`; compiler, assembler, and SandSLAB switches default `ON`; docs default `OFF` |
| Tourchstone | `TOURCHSTONE_BUILD_XBIND=ON`; `TOURCHSTONE_BUILD_DYNLOAD` and `TOURCHSTONE_BUILD_DYNINVOKE` default on only with active qBFD availability |
| XBind | `XBIND_BUILD_TESTS` inherits shared defaults; `XBIND_BUILD_QAMRPP_BINDING=ON`, `XBIND_BUILD_DOMBIND=OFF` |

Sandstone's feature switches are `SANDSTONE_BUILD_COMPILER`,
`SANDSTONE_BUILD_ASSEMBLER`, `SANDSTONE_BUILD_SANDSLAB`, and
`SANDSTONE_BUILD_DOCS`. Disabling the compiler avoids its Boost.JSON dependency;
disabling SandSLAB avoids that subproject's DSLtk dependency.

`*_BUILD_TOOLS=OFF` does not necessarily mean no executables are compiled.
Flintstone, Limestone, and Sandstone can build CLI helpers for tests, and XBind
has its own executable rules rather than a shared tool switch.

## Dependency path resolution

[MLVMDependencies.cmake](../cmake/MLVMDependencies.cmake) retains its historical
filename but resolves dependencies within this separated Libstone checkout:

1. `LIBSTONE_THIRD_PARTY_DIR` defaults to repository `third_party/`.
2. Each dependency has a cached `LIBSTONE_<KEY>_SOURCE_DIR` path.
3. Explicit per-dependency paths override defaults from the root.
4. Historical `MLVM_*` paths seed the new variables only when the new variable
   is not defined; compatibility aliases then mirror the Libstone values.

For example, on a fresh build:

```sh
cmake -S blackstone -B build-blackstone-external \
  -DLIBSTONE_THIRD_PARTY_DIR=/path/to/vendor \
  -DLIBSTONE_LUA_SOURCE_DIR=/path/to/lua-source
```

For Lua, select the directory **containing `lua.h` and `lapi.c`**, which may be
an unpacked release's `src/` directory. A directory of installed headers or a
system Lua library is not a replacement for that source input.

| Source directory variable | Default relative to dependency root |
| --- | --- |
| `LIBSTONE_LUA_SOURCE_DIR` | `lua` |
| `LIBSTONE_METATK_SOURCE_DIR` | `metatk` |
| `LIBSTONE_DPARSER_SOURCE_DIR` | `dparser` |
| `LIBSTONE_SATIE_SOURCE_DIR` | `satie` |
| `LIBSTONE_EQUINOX_NG_SOURCE_DIR` | `equinox-ng` |
| `LIBSTONE_EXOLANGTK_SOURCE_DIR` | `exolangtk` |
| `LIBSTONE_LMDBXX_SOURCE_DIR` | `lmdbxx` |
| `LIBSTONE_KAGUYA_SOURCE_DIR` | `kaguya` |
| `LIBSTONE_SFSEXP_SOURCE_DIR` | `sfsexp` |
| `LIBSTONE_KLIB_SOURCE_DIR` | `klib` |
| `LIBSTONE_TOML_SOURCE_DIR` | `tomlc99` |
| `LIBSTONE_ISOCLINE_SOURCE_DIR` | `isocline` |
| `LIBSTONE_TERMLIB_SOURCE_DIR` | `termlib` |

`LIBSTONE_MEMTKX_SOURCE_DIR` is special: it defaults to
`${LIBSTONE_SATIE_SOURCE_DIR}/third_party/memtkx`, the compatible DomMEMTk API.
The separate upstream `memtkx` checkout has a renamed API and is not an
interchangeable substitute.

XBind additionally exposes `XBIND_QAMRPP_ROOT` for its QaMRpp headers. Sandstone
uses `SANDSTONE_DSL_DIRECTORY` for the external `dsl.lua` runtime; the Python
spec driver accepts `--dsl-dir` and the environment variable of the same name.

Changing the dependency root in an existing cache does not recompute cached
per-dependency paths. Prefer a new binary directory. Inspect cached paths with:

```sh
cmake -LAH -N build
```

## Acquiring sources

Vendor approved snapshots manually, or use the explicit network-fetch helper:

```sh
bash scripts/get-deps.sh --help
bash scripts/get-deps.sh lua qamrpp
```

With no packages, the helper fetches its full inventory, which is broader than
many component builds require. Existing source directories are preserved and
verified by default. `--update` updates existing Git checkouts; `--force` deletes
and replaces them, so do not use it on local work or curated snapshots.

The helper does not pin an API-compatible revision set. Its historical MLVM
build hints are not the aggregate configuration interface for this repository;
use the root commands in [Chapter 2](02-building.md). Fetching is a separate,
deliberate operation: CMake itself never fetches or updates dependencies.

## Limestone revision compatibility

The complete Limestone implementation needs extended Equinox-NG/EkippX APIs,
including `set_observers`, `set_interrupt`, `add_node`, and
`ekippx::MappedExpansion`. Current upstream checkouts do not provide every API
used here. Configure-time file checks establish presence, **not API compatibility**.

Select known-compatible source snapshots explicitly:

```sh
legacy=/path/to/compatible/limestone/third_party
cmake -S limestone -B build-limestone \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLIBSTONE_EQUINOX_NG_SOURCE_DIR="$legacy/equinox-ng" \
  -DLIBSTONE_METATK_SOURCE_DIR="$legacy/metatk"
cmake --build build-limestone --parallel 2
ctest --test-dir build-limestone --output-on-failure
```

The same overrides work with `cmake -S .`. Other required dependencies must
still be populated. There is no automatic machine-specific snapshot search.

To omit optional system facilities, add:

```text
-DLIMESTONE_ENABLE_PERSISTENT_CACHE=OFF
-DLIMESTONE_ENABLE_NATIVE_FFI=OFF
```

These disable LMDB-backed persistence and libffi native invocation respectively;
they do not remove the rest of Limestone's vendor dependencies. In-memory
caching remains available, as do Exolayer callbacks/extensions; native
availability must be checked by the caller.

Next: [First programs and tool workflows](04-workflows.md).
