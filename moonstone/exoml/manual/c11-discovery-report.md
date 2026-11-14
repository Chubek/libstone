# C11 discovery verification report — 2026-11-14

This report records the initial 30-test discovery slice. The subsequent
[normalization-cache continuation](discovery-cache.md#verification-report)
adds implemented persistent graph caching and raises the ON suite to 32 tests.
The open-work and command results below are historical to the initial slice.

## Status and scope

**Implemented, partial P0.1:** real C11 libclang discovery, owned declaration/type
graph, deterministic JSON, compiler target/layout evidence, SHA-256 parsed-input
fingerprints, constrained CLI, separately installable optional SDK, functional
fixtures and pinned real MuLIB inventory comparison. Existing static-binding
APIs and interpreter behavior are unchanged.

**Not complete:** gate A (persistent cache, complete macro provenance/active
defines, compile-commands and broader ABI flags), semantic contract schema,
reviewed contracts, generator ingestion, direct AOT C11 adapters, On1x metadata,
Chisel/Wetstone/Flintstone imports and later integrations. These tests do not
execute MuLIB implementations or prove ownership safety. No safe-export request
is available: discovery JSON always reports `safe_export=false` and unknown
effects. Compiler errors/limits produce partial evidence with `complete=false`.

Public APIs: `<exoml/CDecl.hpp>` (`cdecl::TranslationUnit`, `Type`, `Declaration`,
`describe_translation_unit`) and `<exoml/Extract.hpp>` (`ExtractOptions`,
`ExtractLimits`, `extract_c11`). Extraction requires its separate
`Moonstone::ExoMLExtract` target/package. The ordinary runtime/model serializer
does not link libclang or OpenSSL.

Actual changed paths: `include/exoml/{CDecl,Extract}.hpp`,
`src/{CDecl,Extract}.cpp`, `tools/exoml-bind.cpp`, `CMakeLists.txt`,
`cmake/MoonstoneExoMLExtractConfig.cmake.in`, `tests/{discovery.h,discovery.py,
discovery_probe.c,extractor.cpp,package.cmake,consumer/}`, the three ExoML status/
design/readme files, this manual, and Moonstone's README inventory. All are under
`moonstone/`. No vendor, MuLIB or analysis implementation was changed.

Regression risks: this is a new discovery schema, not a conversion of legacy
binding reflection. Unknown compiler constructs remain explicit. Toolchain
resource/header selection matters; a Clang-derived layout is not automatically
GCC/MSVC ABI equivalence. Export budgets do not constrain Clang's internal
parsing cost; hostile discovery still needs external process/filesystem limits.

## Revisions and dependencies

- LibStone baseline: `a527489294547e3dcae75ed5f62b58650b6746af`; clean before edits.
- Canonical MuLIB: `/mnt/nvme/mulib`, clean commit
  `f6f2eea234f1baa3fe73b8eb49194e7362b76278`, origin
  `git@github.com:Chubek/mulib`. The tests require this exact pin, compare tracked
  source to HEAD, and fingerprint the parsed headers plus manifest. The vendored
  `third_party/mulib` snapshot was not used or combined with it.
- Native GCC/G++ 16.2.1 (`20260810`), target `x86_64-pc-linux-gnu`.
- Native Clang/Clang++/libclang 22.1.8, resources `/usr/lib/clang/22`, target
  `x86_64-pc-linux-gnu`, 64-bit pointers. Compiler-only cross-layout checks use
  `i386-unknown-linux-gnu` and `x86_64-unknown-linux-gnu`.
- OpenSSL Crypto 3.6.4; Python 3.14.7 for independent CLI/inventory tests.

Vendored dependencies are ordinary Git trees in this checkout, not independently
versioned submodules. Exact tree IDs at the LibStone baseline:

| Dependency | Git tree |
| --- | --- |
| cparser-iso11 | `0edbdfe1bf4f70af97c7cbe9c82cb2954bc393a3` |
| ucpp | `676ecc2cf3e6fb340b0238163979464358f41a56` |
| exolangtk | `9f1ac308428f88972049fe758b590ac6dbccd740` |
| libglr | `07b264a44a2cf5e245fda81a765d3e48bc4d376f` |
| metatk | `706f36de28c3d1ea5191a7493f6d5ed61fc3e7d4` |
| unused vendored mulib | `7aa6dce5e05bc573e4d37c82bbc35cc85860a07b` |

`git submodule status` fails on a pre-existing unmapped
`.decomissioned/lapislazuli/.info/binutils` gitlink; no submodule repair was
attempted. Actual sources were inspected: CParser documents incomplete
preprocessed-input support, ucpp is a C99 preprocessor, and ExolangTk's
`ffi_call` implementation returns `FFI_ENOSYS`. None is used as an ABI oracle.
The selected frontend follows MuLIB's libclang AST approach without its broad
implementation-macro emission or raw callback trampolines.

## MuLIB coverage and fingerprints

| Header | Functions | Typedef declarations | Record declarations | Enum declarations | Raw generated / safely reviewed |
| --- | ---: | ---: | ---: | ---: | --- |
| mulib-vec.h | 7 | 1 | 1 | 0 | 0 / 0 |
| mulib-json.h | 20 | 3 | 2 | 1 | 0 / 0 |
| mulib-parsec.h | 25 | 11 | 8 | 3 | 0 / 0 |

All 52 functions match both independently scanned source declarations and the
pinned `Inventory.json` function lists: **zero omitted functions**. Counts are
source declarations, not unique canonical types. Exported type graphs additionally
contain referenced system/dependency types. Header guards are recorded as macros,
not callable symbols; macro expansion/include directive cursors are not public
declarations. No broader MuLIB coverage is implied.

All pointers/callbacks/records remain unsafe as binding inputs until contracts
and verified generators exist. Opaque records are incomplete by design; unions,
bitfields and nonconstant arrays retain explicit adapter requirements. No function
is rejected merely to shrink the inventory, and no C signature is fabricated.

SHA-256:

```text
b2576214db977f01ef73d07059c2877082b57bcb14e5773fb7bc98af591e2a0e  include/mulib-vec.h
e9e233759cfa04eed6f5d05e98ca619118c42b6dc71e1f94a1d0aa882a5585e0  include/mulib-json.h
f697a7723bf797241a069a98f47d8adcc4f7d4742eb5c75713ed4b3d7dd40d42  include/mulib-parsec.h
d5dba9b4902c960af45636f5edcb5c6b658ec53ec5d4cf32986aab846e8356cd  MANIFEST.yaml
90e95c9337749e3dfcce558c66e5b145b7746127b3705f780ac06c9226894192  mudsl/scripts/generate_mulib.py
7a6891260f59882399f49b882b52a10ee9c446efb73c75ac2e754af71f17a49b  mudsl/mull/bridge/Mulib/Inventory.json
```

## Commands and results

Initial baseline (before source edits): **28/28**, GCC Debug; configuration kept
ExoML and Chisel tests enabled and disabled only unrelated Prolific:

```sh
cmake -S moonstone -B build-moonstone \
  -DMOONSTONE_BUILD_TESTS=ON -DMOONSTONE_BUILD_TOOLS=ON \
  -DMOONSTONE_BUILD_EXOML=ON -DMOONSTONE_BUILD_CHISEL=ON \
  -DMOONSTONE_BUILD_PROLIFIC=OFF
cmake --build build-moonstone --parallel 4
ctest --test-dir build-moonstone --output-on-failure
```

Extraction ON, GCC Debug, then Release (replace `debug`/`Debug` with
`release`/`Release` for the second executed configuration): **30/30 each**.

```sh
cmake -S moonstone -B /tmp/opencode/exoml-discovery-debug \
  -DCMAKE_BUILD_TYPE=Debug -DMOONSTONE_BUILD_TESTS=ON \
  -DMOONSTONE_BUILD_TOOLS=ON -DMOONSTONE_BUILD_EXOML=ON \
  -DMOONSTONE_BUILD_CHISEL=ON -DMOONSTONE_BUILD_PROLIFIC=OFF \
  -DMOONSTONE_EXOML_EXTRACT=ON -DMOONSTONE_EXOML_MULIB_ROOT=/mnt/nvme/mulib
cmake --build /tmp/opencode/exoml-discovery-debug --parallel 4
ctest --test-dir /tmp/opencode/exoml-discovery-debug --output-on-failure
```

Clang Debug, ASan/UBSan: **30/30**, no sanitizer failures:

```sh
cmake -S moonstone -B /tmp/opencode/exoml-discovery-clang-san \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DMOONSTONE_BUILD_TESTS=ON -DMOONSTONE_BUILD_TOOLS=ON \
  -DMOONSTONE_BUILD_EXOML=ON -DMOONSTONE_BUILD_CHISEL=ON \
  -DMOONSTONE_BUILD_PROLIFIC=OFF -DMOONSTONE_EXOML_EXTRACT=ON \
  -DMOONSTONE_EXOML_MULIB_ROOT=/mnt/nvme/mulib
cmake --build /tmp/opencode/exoml-discovery-clang-san --parallel 4
ctest --test-dir /tmp/opencode/exoml-discovery-clang-san --output-on-failure
```

Extraction OFF: rebuilt `build-moonstone`, **28/28** again. Public ExoML headers
compile in isolation with `-Wall -Wextra -Wpedantic -Werror` on both compilers.
New extractor/tests use warnings as errors and assertions remain enabled in
Release. `exoml_package` installs and executes independent runtime/extractor
consumers. `ldd` on the extraction-OFF installed consumer shows neither libclang
nor libcrypto. Existing unrelated Chisel/Supersonic/Syren warning diagnostics
remain; no warning suppression was added.

The independent CLI test compiles/runs a real C11 layout probe using the selected
host compiler and compares sizes, alignment and field offsets to libclang. It
checks dependency edits/removal, include-search order, target width and macro
changes without claiming a cache hit. Invalid source, malformed options,
implementation switches, unknown target, missing files and count/depth/byte
exhaustion fail closed. Quoted header filenames are passed directly to Clang,
not interpolated into generated C or a shell.

Clang static analysis passed on `src/Extract.cpp` and `src/CDecl.cpp`:

```sh
clang++ --analyze -std=c++20 -I moonstone/exoml/include -I moonstone \
  moonstone/exoml/src/Extract.cpp -o /tmp/opencode/exoml-extract-analysis.plist
clang++ --analyze -std=c++20 -I moonstone/exoml/include -I moonstone \
  moonstone/exoml/src/CDecl.cpp -o /tmp/opencode/exoml-cdecl-analysis.plist
```

New/changed C/C++ files were clang-formatted and pass its dry-run check.
Local build/CTest logs are `/tmp/opencode/exoml-discovery-{debug,release,
clang-san,off}-{build,ctest}.log` (the initial baseline build was incremental;
the Debug ON configuration was built clean). The sanitizer CTest log includes
the complete 30-test outcome; vendored libclang itself is not sanitizer-built.

The documented extraction interface and `--out` were also executed against real
MuLIB vec, producing `/tmp/opencode/exoml-vec.cdecl.json`. No complete LibStone
root build, Windows/MSVC run, foreign-target execution or On1x compiler test is
claimed.
