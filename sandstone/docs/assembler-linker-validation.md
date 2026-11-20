# Assembler/linker SDK validation

## Revision and scope

Base LibStone revision: `c2f90f62025e32ec903e63b272e3db3569982718`.
All changes are local. No remotes changed or commits pushed.

Vendored Git tree fingerprints at that revision:

| Dependency | Tree |
| --- | --- |
| Lua 5.5.0 | `65275619e7829b569bfb18dea624358aa17dbc91` |
| Kaguya | `fc814bd5d7f214240bd7abd03249dc4f87c48263` |
| metatk | `706f36de28c3d1ea5191a7493f6d5ed61fc3e7d4` |

Compiler/target: GCC 16.2.1 and Clang 22.1.8,
`x86_64-pc-linux-gnu`; Python 3.14.7, SWIG 4.5.1. Vendor sources are unmodified;
`cmake/KaguyaCompat.hpp` adapts Kaguya's legacy GC calls to Lua 5.5.

Delivered:

- SandAS source preprocessing, macro/conditional/repetition/structure/enum/include
  directives, per-invocation inline Lua, retained per-context C SDK registrations,
  checked ArchML `.insn`, and C++ convenience methods.
- Correct string emission, data ranges, expression overflow/precedence,
  unresolved constants, section-relative padding and relocation addends.
- Yellowstone C/C++ script SDK, Lua builders/macros, source includes,
  typed data/fill/assignment/assertion actions, script-defined symbol discovery,
  script-only INPUT and library lookup, entry precedence, and ELF LOAD-page fixes.
- Installed SDK headers/static dependencies, working C11 consumer examples,
  and SWIG Python linking against the new bundled dependencies.
- Missing `compiler/stdspec/dsl.lua` restored. ArchML parser working-directory
  dependency and Clang metadata-literal portability defect fixed. ASan exposed
  an older test fixture's incorrect literal length; the fixture now retains its
  intended embedded bytes using `sizeof`, with its round-trip checks intact.

New APIs are listed in [assembler.md](assembler.md) and
[Yellowstone's extension manual](../../yellowstone/manual/26-extension.md).
The public C option structures remain unchanged. Source callback user data is
borrowed; callers serialize use of each context/registry.

## Results

Baseline before implementation: Sandstone 7 passed / 1 failed (missing DSL);
Yellowstone needed absent-binscript and disabled-pager configuration fixes.
The temporary builds were recreated after the server restart; source work was
retained.

| Configuration | Sandstone | Yellowstone |
| --- | --- | --- |
| GCC Debug, warnings enabled | 9 passed | 15 passed, 1 skipped |
| GCC Release, warnings enabled | 9 passed | 15 passed, 1 skipped |
| Clang Debug, ASan + UBSan + leak detection | 9 passed | 15 passed, 1 skipped |
| GCC Debug, ArchML disabled | 7 passed | n/a |

The Yellowstone skip is `qbfd_source_notes`: its optional local
`binutils/bfd/bfd-in2.h` snapshot is absent. The actual qBFD cross-target fixture
test passes. Yellowstone retains existing unused-function/parameter warnings
in `position_dependenc.cpp` and `symbol.cpp`; warnings were not disabled.
SWIG reports its existing const-pointer setter and Python-keyword warnings.

The new suites check native program exit codes and ELF bytes, C SDK expansion
and linking, Lua state reset and errors, macro defaults/variadics/local labels,
Lua delimiters inside strings/comments, sink failures, malformed expressions,
overflow, region bounds, overlap, and unsupported directives. Native linker
inputs are assembled with the actual host C toolchain.

## Reproduction from the repository root

```sh
cmake -S sandstone -B /tmp/opencode/sandstone-debug -DCMAKE_BUILD_TYPE=Debug \
  -DSANDSTONE_BUILD_TESTS=ON -DSANDSTONE_BUILD_SANDSLAB=OFF
cmake --build /tmp/opencode/sandstone-debug --parallel 3
ctest --test-dir /tmp/opencode/sandstone-debug --output-on-failure

cmake -S yellowstone -B /tmp/opencode/yellowstone-debug -DCMAKE_BUILD_TYPE=Debug \
  -DYELLOWSTONE_BUILD_TESTS=ON -DQOBJFILE_DOMTERM=OFF
cmake --build /tmp/opencode/yellowstone-debug --parallel 3
ctest --test-dir /tmp/opencode/yellowstone-debug --output-on-failure
```

Release used the same commands with `-DCMAKE_BUILD_TYPE=Release` and build
directories `/tmp/opencode/{sandstone,yellowstone}-release`. Sanitizer builds
used `/tmp/opencode/{sandstone,yellowstone}-sanitize`, Debug, and these additional
configuration arguments:

```sh
-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
'-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
'-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
```

Tests ran with `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1`.
CTest records are in each build's `Testing/Temporary/LastTest.log`.
The ArchML-generated standalone header consumer is compiled by its existing
test with `-std=c++20 -Wall -Wextra -Werror -pedantic`; that subprocess is not
sanitizer-instrumented, while the generator and libraries are.

ArchML-off used the Sandstone Debug configuration plus
`-DSANDSTONE_BUILD_ARCHML=OFF` in `/tmp/opencode/sandstone-noarchml`.

Installed C11 consumers were compiled with `-std=c11 -Wall -Wextra -Werror`,
linked against the Release installations and executed from `/tmp/opencode`.
Commands and source files are in [INSTALL.md](../INSTALL.md) and
[the linker extension manual](../../yellowstone/manual/26-extension.md).
The assembler checks exact emitted bytes; the linker-generated native program
exits with status 7.
A combined installed C++20 consumer also compiled with `-Wall -Wextra -Werror`
and executed successfully, using `sandstone::Assembler`, `qld::ScriptSDK` and
`qld::link` in the same process with one bundled Lua library.

```sh
sandstone/bindings/generate-bindings.sh --language python \
  --out /tmp/opencode/sandstone-bindings --build \
  --build-dir /tmp/opencode/sandstone-debug
python3 /tmp/opencode/sandstone-bindings/python/smoke.py
git diff --check
```

Python reports `BINDINGS_OK`.

## Remaining limits and regression risks

- Sandstone writes flat binary/custom JSON, not ELF; no direct Yellowstone
  JSON-object bridge. ArchML `.insn` accepts constants, not symbolic relocations.
- Script expression evaluation is source-ordered, not GNU ld's lazy evaluator.
  Full dynamic linking, GOT/PLT/TLS, program-header scripts, LMA, symbol versions
  and hidden script-symbol visibility remain unsupported. See the script manual
  for rejected commands and exact limits. SDK expansion is an extension point,
  not a replacement for missing native relocation algorithms.
- Existing `.type`/`.size` assembler metadata no-ops remain. GAS/NASM and GNU ld
  compatibility is partial. Expansion diagnostics carry file/line context;
  flattened assembler parse/layout diagnostics do not yet retain an expansion
  source map.
- Callback data must outlive registrations. Preprocessing Lua and linker Lua
  execute ordinary build scripts without a sandbox or execution-time quota.
  Sandstone's separate [post-expansion hook](assembler.md#optional-post-expansion-lua-hook)
  has memory and Lua instruction limits.
- The most sensitive changes are preprocessing order, script-before-input
  discovery, relocation offsets after script-emitted padding, and LOAD permission
  merging when RX/RW sections share a page. Those paths have executable regressions.
- Component builds only: no complete root integration, Windows/MSVC run, full
  SandSLAB validation, or native cross-target execution is claimed.
