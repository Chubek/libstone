# Building libstone

The Stone components now build from this separated `libstone` checkout. CMake
never downloads dependencies; populate `libstone/third_party/` or pass explicit
`LIBSTONE_*_SOURCE_DIR` cache variables for vendored dependencies.

```sh
cmake -S libstone -B build-stone
cmake --build build-stone --parallel
ctest --test-dir build-stone --output-on-failure
```

From inside this repository:

```sh
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The aggregate and individual components can be configured independently:

```sh
cmake -S libstone -B build-stone
cmake -S libstone/sandstone -B build-sandstone
cmake -S libstone/yellowstone/qbfd -B build-qbfd
```

## Options

- `LIBSTONE_BUILD_BLACKSTONE`, `LIBSTONE_BUILD_FIRESTONE`,
  `LIBSTONE_BUILD_LIMESTONE`, `LIBSTONE_BUILD_SANDSTONE`,
  `LIBSTONE_BUILD_YELLOWSTONE`, and `LIBSTONE_BUILD_TOURCHSTONE` select
  components in the aggregate build.
- `TOURCHSTONE_BUILD_DYNLOAD` enables the qBFD-backed dynamic loader and
  `tdynload` inspection tool under `tourchstone/dynload`.
- `TOURCHSTONE_BUILD_DYNINVOKE` enables the libffi/dyncall-style foreign-call
  library and `dinvoke` tool under `tourchstone/dyninvoke` (ELF/Mach-O/PE with
  AMD64/AArch64/RISC-V layouts from Sandstone ISA data).
- `TOURCHSTONE_BUILD_XBIND` enables the XBind spec runner, JSON IR, signature
  probes and Lua/DomLuaBind generator under
  `tourchstone/xbind`. The committed `.xbind` specs target D, Go, Java, Lua,
  NodeJS, OCaml, Python, Ruby, and Rust; see `tourchstone/xbind/MANUAL.md`.
- `LIBSTONE_BUILD_TESTS`, `LIBSTONE_BUILD_TOOLS`, and
  `LIBSTONE_ENABLE_WARNINGS` are the top-level defaults. Standalone tests
  default to `BUILD_TESTING`.
- Each component has its own `*_BUILD_TESTS` option. Components with tools also
  have `*_BUILD_TOOLS`. Tests may build CLI executables as test helpers even
  when tool installation is disabled.
- `DOMTERM_BUILD_STDLIB=ON` builds the optional Termscript standard library
  using `third_party/tomlc99` and `third_party/isocline`.

Set `LIBSTONE_THIRD_PARTY_DIR` on the first configure to relocate all dependency
defaults. Individual `LIBSTONE_<NAME>_SOURCE_DIR` cache variables override
specific checkouts, including `LIBSTONE_METATK_SOURCE_DIR`,
`LIBSTONE_DPARSER_SOURCE_DIR`, `LIBSTONE_LUA_SOURCE_DIR`, and
`LIBSTONE_KAGUYA_SOURCE_DIR`. Historical `MLVM_*` cache names are still accepted
as compatibility aliases. When changing the root in an existing build, clear the
cached per-dependency paths or use a fresh build.

Sandstone and Yellowstone use `metatk/DSLtk/DSLtk.hpp`, replacing the old
DomDSL copy. VMWeave builds Lua from sources and exports that library with
Limestone; installed consumers do not need a separately installed Lua.
Satie and VMWeave's allocator tests use the compatible DomMEMTk snapshot under
`third_party/satie/third_party/memtkx` by default. The separate upstream
`memtkx` checkout has a renamed API. The `exolayer-manifest` check and the
`19_exolayer.md` install are enabled only when the configured ExolangTk
checkout provides them; upstream checkouts without
`manifests/Exolayer-Modules.yaml` skip that test instead of failing.

System build prerequisites include a C/C++ compiler and CMake 3.20 or newer.
Parser generation requires Perl. Python is needed for the Python test suites.
Sandstone requires zlib and optionally uses zstd. Limestone uses libffi and
LMDB unless `LIMESTONE_ENABLE_NATIVE_FFI` or
`LIMESTONE_ENABLE_PERSISTENT_CACHE`, respectively, is disabled. SWIG Python
bindings and D MachineIR tests retain their separate optional toolchains.

## Limestone dependency revisions

The complete Limestone backend needs the extended Equinox-NG and EkippX APIs
used by its sources: `set_observers`, `set_interrupt`, `add_node`, and
`ekippx::MappedExpansion`. The current upstream Git checkouts do not provide
all of these APIs. Configure-time checks verify source files, not API
compatibility. Use compatible snapshots through explicit overrides. For example:

```sh
legacy=/path/to/compatible/limestone/third_party
cmake -S libstone/limestone -B build-limestone \
  -DLIBSTONE_EQUINOX_NG_SOURCE_DIR="$legacy/equinox-ng" \
  -DLIBSTONE_METATK_SOURCE_DIR="$legacy/metatk"
cmake --build build-limestone --parallel
ctest --test-dir build-limestone --output-on-failure
```

The same overrides work with the aggregate build. They are explicit
configuration inputs; no machine-specific lookup is built into CMake.

## Dependencies

The default dependency root is `libstone/third_party`. Packagers may vendor
dependencies there, or point individual `LIBSTONE_*_SOURCE_DIR` variables at
external read-only checkouts. Configure-time checks verify source files only;
they do not fetch or update dependencies.
