# Verification

Linux x86-64: GCC 16.2.1, Clang 22.1.8, CMake 4.4.3, Python 3.14.
Run from the checkout root:

```sh
cmake -S cornerstone/Amalgam8 -B build/am8-gcc -DAMALGAM8_BUILD_TESTS=ON
cmake --build build/am8-gcc --parallel 3
ctest --test-dir build/am8-gcc --parallel 3 --output-on-failure
cmake -S cornerstone/Amalgam8 -B build/am8-clang -DAMALGAM8_BUILD_TESTS=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/am8-clang --parallel 3
ctest --test-dir build/am8-clang --parallel 3 --output-on-failure
python3 cornerstone/Amalgam8/tools/catalog_audit.py --probe-firestone
```

The Amalgam8 suites cover syntax and module identity, stage limits and
provenance, semantic checks, linked C11 and C++ consumers, the interpreter and
native extension examples, an installed compiler client, dependency packaging,
and native interoperability (13/13 on GCC and Clang). The catalog audit validates 331 records against
the checked-out public API and metadata. A catalog match does not certify
that the referenced implementation is installed or appropriate for the
consumer target.

## Existing repository checks

```sh
cmake -S firestone -B build/am8-firestone -DFIRESTONE_BUILD_TESTS=ON \
  -DFIRESTONE_BUILD_DSL_EXAMPLES=ON
cmake --build build/am8-firestone --parallel 4
ctest --test-dir build/am8-firestone --output-on-failure

cmake -S . -B build/am8-repo -DLIBSTONE_BUILD_TESTS=ON -DLIBSTONE_BUILD_AMALGAM8=ON
cmake --build build/am8-repo --target amalgam8 --parallel 2
cmake --build build/am8-repo --target xbind_core_tests xbind_cli \
  xbind_qamrpp_module_test qamrpp_xbind moonstone_test_catalog --parallel 4
ctest --test-dir build/am8-repo -R '^(xbind\.|moonstone\.catalog$)' --output-on-failure
```

Firestone passed 57/57 including all 22 LibDSL tests/examples. The top-level
Amalgam8 target built successfully. The focused XBind/Moonstone run passed 7/8
after building `qamrpp_xbind`. Known checkout failures:

- `cmake --build build/am8-repo --parallel 4` also fails with
  `LIBSTONE_BUILD_AMALGAM8=OFF`: `limestone/tunah/tunah.cpp` expects
  Equinox-NG `set_observers`, `set_interrupt`, `add_node`, and
  `OperationInterrupted`. See the root README for compatible vendor overrides.
- `xbind.codegen` fails even with Amalgam8 disabled: its expected repository
  inventory omits `flintstone/Flintstone.xbind` and
  `.decomissioned/yellowstone/Yellowstone.xbind`.
- `cmake --build build/am8-repo --target moonstone_test_integration` requires
  `scripts/aurocks.pl` to generate ExoML's parser; that file is absent from
  the checkout. Amalgam8's Moonstone example uses the real header-only Core
  parser and does not depend on ExoML.

## Platform and dependency gaps

- CLI publication was tested on Linux using `mkstemp`, `fdopen`, and
  same-filesystem `rename`. macOS/BSD POSIX runs and an MSVC embedding-only
  build (`AMALGAM8_BUILD_TOOLS=OFF`, C++20) were not tested. Windows
  executable publication is unsupported.
- Firestone's `BigInt`, `BigFloat`, `Complex128`, and `LibDSL` were tested as
  installed consumers; only the first three have bundled snapshots.
  Other catalog entries are not individually certified as generated
  Amalgam8 consumers. Use the native `Firestone::...` target; its own
  conditional include graph and link usage requirements remain authoritative.
  In particular, the OpenMP path needs `FIRESTONE_ENABLE_OPENMP=ON` and a
  discoverable `OpenMP::OpenMP_CXX`; the AArch64 SIMD path needs an AArch64
  compiler/target with `__ARM_NEON` and an AArch64 runtime. Linux-specific
  process/scheduling paths use `__linux__`; their non-Linux paths were not
  cross-tested.
- Optional Tourchstone native loader/invoker verification needs active
  Yellowstone qBFD and `TOURCHSTONE_BUILD_DYNLOAD=ON` or
  `TOURCHSTONE_BUILD_DYNINVOKE=ON` (the latter also uses Sandstone ISA data).
  DomBind packages require their selected language SDK and generated C API
  headers. They are cataloged, not included in the generated adapter test.
- Moonstone ExoML extraction needs `MOONSTONE_EXOML_EXTRACT=ON`, libclang, and
  OpenSSL Crypto. Prolific needs `MOONSTONE_BUILD_PROLIFIC=ON`, LMDB and its
  Lua/Kaguya/lmdbxx dependencies; QMR needs `MOONSTONE_BUILD_QMR=ON` and
  vendored Lua. These optional named integrations have diagnostics, not
  fallback implementations. Other unverified entries and their build targets
  are identified individually in the [catalog](catalog.md).

The compiler does not discover an arbitrary C ABI from headers, infer native
dependencies from opaque fragments, or sandbox an embedding callback. Use
explicit declarations and native consumer build requirements for those cases.
