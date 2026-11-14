# Exotract — static C11 single-header contracts for Moonstone

Exotract is a **C++20 library and CLI** with an optional **vendored Lua/Kaguya**
frontend, **LMDB (lmdbxx)** storage, **ucpp** preprocessing probe, and **cparser-iso11**
compatibility probe. Its typed declaration authority is **Moonstone ExoML**.
It models the special build contract of a C11 single-header library rather than
implementing a second C type system. No vendor subtree is copied into this archive.

## Features actually implemented

- Declarative library profiles: header, dependencies, include guards, implementation
  defines, exactly-one-translation-unit policy, C11 compiler profile, include/define/undefine set.
- Bounded, concurrent stdout/stderr draining, no-shell POSIX process invocation,
  process timeout, output caps, and diagnostic errors.
- Header preprocessing (GCC/Clang or optional vendored ucpp), independent C11 syntax check,
  optional CParser compatibility probing, and direct or CLI ExoML CDecl v2 ingestion.
- Typed function/typedef/enum/record/variable inventory from ExoML CDecl, retaining
  raw CDecl JSON so downstream binders can reconstruct full types.
- ExoML `ContractBundle` validation/normalization via the linked SDK, or
  `exoml-bind validate` in standalone configurations. Lua binders can attach a
  validated sidecar with `Bind({contract='api.exocontract.json'})`; this preserves
  annotations and trust claims without authorizing safe exports.
- Generated implementation C translation unit for header-only libraries.
- Conservative versioned JSON manifest and On1x *import manifest* (not generated
  executable On1x syntax). Until reviewed contracts and target lowering exist,
  every discovered symbol remains unsafe/unknown-effect.
- Optional LMDB-backed global key/value registry and normalized declaration cache.
  Cache keys incorporate preprocessed bytes, header/config/compiler identity,
  profile and the complete compiler-derived CDecl (including provenance files);
  a cache hit still performs fresh preprocessing, syntax checking and ExoML
  extraction. It is not an
  authenticated source attestation or complete compiler-native AST validation.
- Lua module `require('exotract')`: `exolib.new`, `Preprocess`, `Parse`, `Cache`,
  `Bind`, `Manifest`, `$GLOBAL{KEY}`, structured executable fallbacks. User-defined
  binder scripts return `function(internals) -> internals`.

## Integration / builds

Enable `MOONSTONE_BUILD_EXOTRACT=ON` in the Moonstone build; its CMake integration
adds Exotract **after ExoML**. `Moonstone::ExoMLExtract` is auto-detected
when configured. The ordinary `moonstone_exotract` C++ library has no mandatory
Lua/LMDB/ucpp/CParser dependency.

Standalone minimal build (works even without LibStone vendor libraries):

```sh
cmake -S moonstone/exotract -B build-exotract \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DEXOTRACT_ENABLE_LUA=OFF -DEXOTRACT_ENABLE_LMDB=OFF \
  -DEXOTRACT_ENABLE_UCPP=OFF -DEXOTRACT_ENABLE_CPARSER=OFF \
  -DEXOTRACT_ENABLE_EXOML=OFF
cmake --build build-exotract -j4
ctest --test-dir build-exotract --output-on-failure
build-exotract/exotract inspect --name Fixture \
  --header moonstone/exotract/tests/fixture.h --compiler clang
```

Integrated optional components are found in `third_party/lua`,
`third_party/kaguya`, `third_party/lmdbxx`, `third_party/ucpp` and
`third_party/cparser-iso11`. LMDB also requires a system `lmdb.h` and library.
The compiler-validated ExoML discovery backend needs ExoML's own opt-in libclang
configuration (`-DMOONSTONE_EXOML_EXTRACT=ON`) and its dependencies.
Enable `-DEXOTRACT_ENABLE_CPARSER=ON` to try the vendored CParser probe; its
upstream README documents incomplete preprocessed-file handling, so the probe
is not an ABI authority. The optional C99-oriented ucpp executable can fail for
C11 conditionals; Exotract then verifies with the selected C11 compiler.

For the all-vendor integration and pinned MicroJS tests:

```sh
cmake -S moonstone -B build-exotract-moonstone -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DMOONSTONE_BUILD_EXOTRACT=ON -DMOONSTONE_EXOML_EXTRACT=ON \
  -DMOONSTONE_BUILD_TESTS=ON -DMOONSTONE_BUILD_PROLIFIC=OFF \
  -DEXOTRACT_ENABLE_CPARSER=ON \
  -DEXOTRACT_MULIB_ROOT=/path/to/canonical/mulib \
  -DEXOTRACT_MULIB_REVISION=345aae537286471e0b3439b8b80961b17110aa5c
cmake --build build-exotract-moonstone -j4
ctest --test-dir build-exotract-moonstone --output-on-failure
```

Tests resolve Clang's resource directory explicitly (override with
`EXOTRACT_TEST_RESOURCE_DIR`). SDK callers set `Config::clang_resource_dir`;
CLI users pass `--resource-dir`. Missing compiler resources fail closed.
`--preprocessor compiler` bypasses the optional ucpp probe; `--parser compiler`
selects the configured `--exoml-bind` subprocess instead of linked discovery.
Neither option converts preprocessor/CParser output into typed declarations.
An installed SDK is supported through `CMAKE_PREFIX_PATH`; standalone tests can
use `EXOTRACT_TEST_EXOML_BIND=/prefix/bin/exoml-bind`.

`exotract inspect` reports `complete:false` (no usable binding) if the typed
ExoML extractor is absent. Syntax checking can still succeed. For standalone
use with an already built ExoML tool, supply:

```sh
build-exotract/exotract inspect --name Fixture \
  --header moonstone/exotract/tests/fixture.h \
  --compiler clang \
  --exoml-bind /path/to/exoml-bind \
  --resource-dir "$(clang -print-resource-dir)"
```

`exotract emit-source --impl-define MULIB_IMPL_MICROJS --impl-define MULIB_IMPL_MINIVM`
creates a C11 implementation TU. **Only one TU** must compile with these defines.
Do not pass them to declaration discovery. For installed MuLIB use `--include /path/to/mulib/include`.
The source `.c` file must be compiled with the required compiler flags/dependencies.

## Lua usage

See `contracts/mulib/microjs.lua` and `examples/fixture.lua`.

```sh
build-exotract/exotract lua moonstone/exotract/contracts/mulib/microjs.lua \
  --cache-dir /private/lmdb/exotract \
  --global MULIB_KERNEL_DIR=/path/to/mulib/include \
  --global MULIB_DOC_DIR=/path/to/mulib/manual \
  --global EXOTRACT_ROOT=/path/to/libstone/moonstone/exotract \
  --global EXOML_BIND=/path/to/exoml-bind \
  --global C_COMPILER=clang \
  --global CLANG_RESOURCE_DIR=/path/to/clang/resource \
  --global MULIB_REVISION=345aae537286471e0b3439b8b80961b17110aa5c \
  --global MULIB_MANIFEST=/path/to/mulib/MANIFEST.yaml
```

The Lua execution layer loads a reduced set of libraries without `os`, `io`,
`debug`, `dofile`, `loadfile` or filesystem package search. **It is NOT a
security sandbox.** The host still reads requested scripts and launches chosen
compilers. Only load trusted contracts, or isolate execution externally.

## Trust model / deliberate non-features

- CDecl `complete:true` means declarations were reported complete, **not** that
  ownership and effects are verified. Every raw binding remains unsafe.
- No arbitrary C11 ABI is guessed, no verified On1x AST/source code is generated,
  no automatic pointer ownership or callback trampoline is fabricated.
- `Bind({safe_exports='reviewed_only'})` fails closed until ExoML-reviewed
  semantic contracts can be propagated into each export. The working binder
  policy is `unsafe_explicit`, which emits metadata only.
- Dynamic FFI and native code execution are intentionally outside scope.
- A bare compiler's preprocessor output is not a faithful ExoML type graph.
- Fallback parser stdout must be valid ExoML CDecl v2 JSON, not plain C.
- No Windows subprocess implementation. POSIX process budgets don't confine
  child memory, filesystem access, or subprocess trees; use external isolation.
- Optional ucpp/CParser helpers currently reference build-tree executables;
  independent installed deployment of those probes remains open. Installed
  ExoML discovery and the exported C++ SDK have executable consumer tests.

## Status

The integration suite exercises real Lua 5.5, LMDB, both vendor probes, pinned
MicroJS and installed SDK consumers. See [STATUS.md](STATUS.md) for measured
counts and Clang sanitizer scope. Safe adapters and On1x AST lowering remain open.
