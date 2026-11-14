# Exotract implementation and verification status

## Implemented in this continuation

- Linked ExoML extraction and canonical `ContractBundle` validation, with a CLI
  fallback for standalone builds. `Bind({contract=path})` attaches normalized
  sidecar metadata only after validation succeeds; exports remain unsafe.
- Explicit CLI preprocessing/parser selection, preserved CParser stdout/stderr
  diagnostics, and separate successful/unsupported vendor-probe tests.
- Compiler-selected test subprocesses and explicit Clang resource configuration.
  Fixed sibling-CMake-scope assumptions and restored the missing discovery test.
- Cross-process LMDB cold/warm/macro/header/provenance invalidation tests,
  independent writers, corruption and actual database-file permission denial.
- Pinned MicroJS Lua discovery and generated single-TU C11 integration: owned
  values, roots surviving collection, callbacks, reentrant-eval rejection,
  teardown and duplicate implementation linkage rejection.
- Installed/relocated CMake target consumer and strict standalone public-header
  compilation. Fixed empty-build-configuration handling in the package test.
- Narrow ExoML portability repair: recursive JSON value special members are
  defined after the type is complete, fixing Clang/libstdc++ compilation.

Public C++ API signatures and artifact schema v2 are preserved. New CLI flags:
`--parser`, `--preprocessor`. New optional Lua binder input: `contract`; output:
`Internals.contract`. MicroJS's Lua profile now requires `MULIB_REVISION` and
`MULIB_MANIFEST`; `C_COMPILER` selects its compiler.

## Measured verification

LibStone revision: `91293264a531603dfd2319e64b3c1aa40a609783` plus local changes.
Exotract and the Moonstone opt-in integration were already uncommitted at start.
Linux x86-64; Clang/libclang **22.1.8**, target `x86_64-pc-linux-gnu`, resource
directory `/usr/lib/clang/22`, libstdc++ 16, Lua **5.5.0**, LMDB **0.9.35**.
Libclang is the system `/usr/lib/libclang.so`, not a vendored Clang build.

| Configuration | Result |
| --- | --- |
| Clang Debug, full Moonstone, all Exotract vendors, ASan/UBSan | **47/47 passed** |
| Clang Release, full Moonstone, all Exotract vendors | **47/47 passed** |
| Clang Release, standalone Exotract built against installed Clang ExoML SDK | **13/13 passed** |
| Clang Debug, standalone core, every optional component OFF | **7/7 passed** |

Sanitizer flags: `-fsanitize=address,undefined -fno-omit-frame-pointer` for C and
C++. ExoML, Exotract, vendor adapters, Lua callbacks and the generated MicroJS
implementation/consumer are built with these flags. No suppression or disabled
leak check was used. Existing unrelated Moonstone/vendor compiler warnings remain;
public header checks use `-Wall -Wextra -Wpedantic -Werror`.

The initial baseline was 7/8 standalone and 37/43 integrated tests: resource
configuration, a missing test file and an incorrect permissions fixture failed.
Before the restart, GCC Debug/Release and mixed Clang/installed-GCC-SDK checks
passed; those results predate the final portability/contract work. Final work
uses Clang throughout. The restart removed temporary logs/builds, not sources.

Reproduce the full instrumented configuration from the repository root:

```sh
cmake -S moonstone -B build-exotract-clang-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DMOONSTONE_EXOML_EXTRACT=ON -DMOONSTONE_BUILD_EXOTRACT=ON \
  -DMOONSTONE_BUILD_TESTS=ON -DMOONSTONE_BUILD_PROLIFIC=OFF \
  -DEXOTRACT_ENABLE_CPARSER=ON \
  -DEXOTRACT_MULIB_ROOT=/mnt/nvme/mulib \
  -DEXOTRACT_MULIB_REVISION=345aae537286471e0b3439b8b80961b17110aa5c
cmake --build build-exotract-clang-sanitize -j2
ctest --test-dir build-exotract-clang-sanitize --output-on-failure
```

Lua, LMDB, ucpp and linked ExoML default ON. The Release configuration uses
`build-exotract-clang-release`, `CMAKE_BUILD_TYPE=Release` and omits sanitizer
flags. The core-only configuration uses `build-exotract-clang-core` and sets
`EXOTRACT_ENABLE_{EXOML,LUA,LMDB,CPARSER,UCPP}=OFF` individually. CTest logs are
retained in each build's `Testing/Temporary/LastTest.log`; current command logs
are `/tmp/opencode/exotract-clang-{configure,build,ctest}.log`.

Release and installed-SDK commands executed:

```sh
cmake -S moonstone -B build-exotract-clang-release \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DMOONSTONE_EXOML_EXTRACT=ON -DMOONSTONE_BUILD_EXOTRACT=ON \
  -DMOONSTONE_BUILD_TESTS=ON -DMOONSTONE_BUILD_PROLIFIC=OFF \
  -DEXOTRACT_ENABLE_CPARSER=ON -DEXOTRACT_MULIB_ROOT=/mnt/nvme/mulib \
  -DEXOTRACT_MULIB_REVISION=345aae537286471e0b3439b8b80961b17110aa5c
cmake --build build-exotract-clang-release -j2
ctest --test-dir build-exotract-clang-release --output-on-failure
cmake --install build-exotract-clang-release \
  --prefix /mnt/nvme/libstone/build-exotract-clang-release/sdk --component ExoML
cmake -S moonstone/exotract -B build-exotract-clang-sdk \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH=/mnt/nvme/libstone/build-exotract-clang-release/sdk \
  -DEXOTRACT_TEST_EXOML_BIND=/mnt/nvme/libstone/build-exotract-clang-release/sdk/bin/exoml-bind \
  -DEXOTRACT_ENABLE_CPARSER=ON -DEXOTRACT_MULIB_ROOT=/mnt/nvme/mulib \
  -DEXOTRACT_MULIB_REVISION=345aae537286471e0b3439b8b80961b17110aa5c
cmake --build build-exotract-clang-sdk -j2
ctest --test-dir build-exotract-clang-sdk --output-on-failure
```

The core-only executable also passed `tests/discovery.py` with the installed
`exoml-bind`, linked=0, LMDB=0 and `/usr/bin/clang`: this separately exercises
the subprocess-only contract validator fallback without linking the SDK.

## MuLIB coverage and provenance

Canonical checkout: `/mnt/nvme/mulib`, commit
`345aae537286471e0b3439b8b80961b17110aa5c`. Tests compare the two headers and
manifest to that commit's Git objects before using them. Unrelated On1x work in
that checkout is preserved. No vendored MuLIB snapshot is combined with it.

| Input | SHA-256 |
| --- | --- |
| `include/mulib-microjs.h` | `655a2a572ae113ef1183e425932ff172f0ae549096de7c59d6cb53017bfcef05` |
| `include/mulib-minivm.h` | `0e0a002d88e8b37892744a6c4547322af0e52c8019b9df5e8df9dd896ad6cefe` |
| `MANIFEST.yaml` | `d5dba9b4902c960af45636f5edcb5c6b658ec53ec5d4cf32986aab846e8356cd` |

MicroJS: **15/15 public functions discovered and exported as unsafe metadata**,
zero omitted functions, **zero reviewed safe bindings**. Callback typedef and
owned result declarations are retained. MiniVM supplies the implementation
dependency; no MiniVM facade coverage is claimed. Both `MULIB_IMPL_MICROJS` and
`MULIB_IMPL_MINIVM` appear exactly once in the generated TU. The native driver
links no ExoML interpreter. Tests exercise behavior, not a client ownership proof.

Vendor Git tree identities at the LibStone revision (no vendor edits):

| Component | Tree |
| --- | --- |
| Lua | `65275619e7829b569bfb18dea624358aa17dbc91` |
| Kaguya | `fc814bd5d7f214240bd7abd03249dc4f87c48263` |
| lmdbxx | `a476adc6cb83f6d16c4f67803c9d49c215a6edbc` |
| ucpp | `676ecc2cf3e6fb340b0238163979464358f41a56` |
| cparser-iso11 | `0edbdfe1bf4f70af97c7cbe9c82cb2954bc393a3` |

## Remaining limitations

- ExoML contract validation checks its current structural/semantic invariants;
  complete source-signature/ABI matching, review authentication, safe adapters
  and ownership-aware On1x AST lowering are still open. `reviewed_only` always
  rejects, even with a supplied sidecar. Unknown effects stay unknown.
- CParser rejects `_Static_assert` and some compiler-expanded system-header
  constructs. ucpp lacks the C11 conditional profile and default system include
  discovery. Their failures retain diagnostics and never fabricate declarations.
- Installed C++ SDK targets work, but optional ucpp/CParser helper paths still
  reference the build tree; independently deployed installed probes are open.
- Cache hits still re-extract; this is integrity-checked reuse, not parse-free
  caching or authenticated provenance. LMDB failures cannot authorize exports.
- No Windows, non-Linux, full LibStone-root or broad cross-target execution claim.
  The 32-bit profile test checks compiler evidence only. Prolific is disabled in
  the reported Moonstone integration configurations.
