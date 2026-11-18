# Portable backend registration manifest — 2026-11-19

Base revision: `934b6ed029bfea28bbf23b5dc77c476154b0dd0d`. Tested on Linux
x86-64 with GCC 16.2.1 and Clang 22.1.8. No Windows or Darwin execution tested.

`qld-backend-manifest` exercises `qld_backend_manifest` from a C11 translation
unit. It checks deterministic call-order records, length-delimited source,
grammar and uniqueness in both columns, exact size/count boundaries, malformed
and bytecode inputs, disabled libraries, and instruction/allocation exhaustion.
Failures after a successful registration still return no output; subsequent
runs get a fresh Lua state. Existing layout scripts continue to use their own
Lua runtime.

From the Yellowstone source directory, GCC Debug, tools enabled, all default
bridges OFF, warnings as errors:

```sh
export TMPDIR=/mnt/nvme/mulib/build
B="$TMPDIR/yellowstone-backend-manifest-debug"
cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Debug -DQOBJFILE_DOMTERM=OFF \
  -DQOBJFILE_WERROR=ON -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON
cmake --build "$B" --parallel 6 --target \
  qld-backend-manifest-test qld-script-sdk-test qld-frontend-test qobjld
ctest --test-dir "$B" --output-on-failure \
  -R '^qld-(backend-manifest|script-sdk|frontend|script-executable)$'
```

All 4 passed. Building the native and format test drivers also gave **7/7**
for `ctest --test-dir "$B" --output-on-failure -R '^qld-'`. The manifest test
was also compiled with GCC `-std=c11 -Wall -Wextra -Werror -pedantic`,
linked directly with `libqld.a` and `liblibstone_script_lua.a`, and passed
with `PATH=/nonexistent` and `QLD_TOOLCHAIN_BACKEND_ACTIVE=1`.

Clang with ASan, UBSan and leak detection, tools disabled:

```sh
B="$TMPDIR/yellowstone-backend-manifest-san"
cmake -S . -B "$B" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined \
  -DQOBJFILE_DOMTERM=OFF -DQOBJFILE_WERROR=ON \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=OFF
cmake --build "$B" --parallel 5 --target \
  qld-backend-manifest-test qld-script-sdk-test qld-frontend-test
env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir "$B" --output-on-failure -R '^qld-(backend-manifest|script-sdk|frontend)$'
```

All 3 passed. Manifest generation alone emits no object or image; target
support and native final linkage are the responsibility of its consumer.

---

# Native dynamic ELF validation — 2026-11-18

Base revision: `d4a073a57c06b55fe91b7c059846e73f2f7a3326`; initial
worktree clean. Linux x86-64, GCC 16.2.1, Clang 22.1.8, Python
3.14.7. No LLVM/LLD library dependency or vendor modification was introduced.

## Implemented and exercised

- `DynamicElf.cpp` emits in-process little-endian ELF64 x86-64 shared objects,
  PIEs and fixed-address dynamic executables. Outputs contain loader-consumable
  PHDR/INTERP/LOAD/DYNAMIC/STACK/RELRO program headers, dynamic symbols and
  strings, SysV/GNU hashes, GOT/PLT, RELA and dynamic tags.
- PIC handling covers GOTPCREL/GOTPCRELX/REX_GOTPCRELX, PLT32, local PC32/PC64,
  writable absolute references, RELATIVE, GLOB_DAT and JUMP_SLOT. Invalid
  address-dependent 32-bit/text forms fail with a PIC diagnostic.
- ELF DSO inputs participate in symbol resolution without having their linked
  sections copied. `DT_NEEDED` prefers parsed `DT_SONAME`; `-L/-l`, shared
  library preference, `--as-needed`, SONAME, RUNPATH/RPATH, hash style,
  bind-now, RELRO, init/fini arrays and protected/hidden/weak symbols are
  covered.
- `PT_PHDR` is emitted for runtime-loader address discovery. A real glibc loader
  executes both native PIE and dynamic ET_EXEC fixtures. `ctypes.CDLL` loads
  native DSOs and verifies constructors, imported function/data, relative
  pointers and weak unresolved behavior.
- CLI, strict C11, C++ one-shot, C one-shot and incremental APIs all select the
  native emitter with a NULL backend executable. `PATH` is invalid and
  `QLD_TOOLCHAIN_BACKEND_ACTIVE=1` during native tests, proving no subprocess
  linker is reached. The all-bridges build separately preserves explicit
  `--dynamic` compatibility behavior.
- qBFD now exposes ELF SONAME and distinguishes STV_PROTECTED from hidden
  visibility so the linker can preserve protected exports and local binding.

## Verified commands

```sh
cmake --build /tmp/opencode/qld-native-debug --parallel 5
ctest --test-dir /tmp/opencode/qld-native-debug --output-on-failure

cmake --build /tmp/opencode/qld-native-bridges --parallel 5
ctest --test-dir /tmp/opencode/qld-native-bridges --output-on-failure

cmake --build /tmp/opencode/qld-native-sanitize --parallel 5
env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/opencode/qld-native-sanitize --output-on-failure
```

| Configuration | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| GCC Debug, native defaults, warnings as errors | 19 | 0 | 1 |
| Clang Release, all explicit bridges ON, warnings as errors | 21 | 0 | 1 |
| Clang ASan/UBSan/leak detection, native defaults | 19 | 0 | 1 |

The skip is the existing `qbfd_source_notes` test: its binutils source snapshot
is unavailable. `qld-native-dynamic` is the new bridge-free runtime regression;
the existing external `qld-dynamic` suite also passes in the all-bridges
configuration.

An installed strict C11 consumer was compiled from `examples/dynamic-sdk.c`
against `/tmp/opencode/qld-dynamic-install`, run with an invalid `PATH` and the
subprocess recursion guard set, and produced a DSO whose exported `leaf()`
returned 7 through `ctypes.CDLL`.

## Deliberate limits

Native dynamic output is currently ELF64 little-endian x86-64 only. TLS, IFUNC,
symbol versions, copy relocations, COMDAT/LTO, dynamic linker scripts and
unsupported relocation encodings fail explicitly. Non-PIE dynamic ET_EXEC is
covered for function/GOT imports but does not implement copy relocations for
traditional direct external-data references. The existing external ELF bridge
remains the opt-in path for those features and compiler-driver parity. No
non-Linux loader or cross-architecture dynamic runtime claim is made.

---

# Previous native-default static-slice validation — 2026-11-18

Base revision: `5d1721a422d6310f2727d6f04b1d6bac58904e07`; initial worktree clean.
Linux x86-64, GCC 16.2.1, Clang 22.1.8, Python 3.14.7. No vendor changes or
LLVM/LLD library dependency was introduced.

## Native implementation and regression evidence

- Backend format selection no longer selects an external process implicitly.
  Static ELF `-l`, `--static`, explicit format APIs and response files use
  native discovery/layout/relocation/emission.
- `NativeImage.cpp` implements fixed-base PE x86/x64 and Mach-O x86-64 images
  with shared archive/symbol discovery and format-specific relocation/output.
  `NativeOptions.cpp` rejects unsupported options instead of forwarding them.
- Archive-only links establish architecture/class/endian from the selected
  member, rather than the default x86-64 geometry. Native library resolution
  supports `-l:filename` and never substitutes a `.so` for a static archive.
- All three external compatibility bridges default OFF. Even with them ON,
  only an explicit executable/API linker or `--dynamic` opts into a subprocess.
  Existing dynamic and cross-format bridge tests now explicitly request it.
- The new regression failed against the baseline: `--static -lanswer` reached
  `runProcess` and tripped its recursion guard. `qld-native` now tests CLI,
  C11, C++, format-specific and incremental APIs with PATH empty and that guard
  set, in both bridges-OFF and bridges-ON builds.
- Independent Python image mapping executes freestanding PE/Mach-O x86-64
  code, including calls, data/BSS, absolute pointers and local symbols.
  SIGNED_1/_2/_4 regressions exposed and fixed double-counted Mach-O immediate
  bias. ELF executables execute through Linux's actual loader; ELF partial
  links are relinked and executed. ELF32 archive-only and PE32 images are
  structurally checked, not executed.
- Negative cases covered unsupported shared/PIE/partial modes, formats and
  emulations, duplicate/undefined symbols, output collisions, TLS, COFF
  directives/COMDAT, unsupported Mach-O GOT relocations and oversized images.
  Failed links preserve the existing destination.

This is a **limited native static slice**, not feature parity with the former
automatic external backends. At that revision native shared/PIE/dynamic imports
were still unimplemented. PE COMDAT/weak/TLS/CRT,
Mach-O ARM64/unwind/initializers/signing,
and other unsupported relocations require further native work. The complete
supported subset and compatibility opt-in are documented in `README.md`.
No Windows/Darwin OS-loader execution, full root build or cross-host build is
claimed. The independent mapping test is not a replacement for those runners.

## Commands and results

Baseline: `ctest --test-dir /tmp/opencode/qld-debug --output-on-failure`:
**19 passed, 0 failed, 1 skipped**.

```sh
cmake -S yellowstone -B /tmp/opencode/qld-native-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON \
  -DQOBJFILE_DOMTERM=OFF -DQOBJFILE_WERROR=ON
cmake --build /tmp/opencode/qld-native-debug --parallel 5
ctest --test-dir /tmp/opencode/qld-native-debug --output-on-failure

cmake -S yellowstone -B /tmp/opencode/qld-native-bridges \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON \
  -DQOBJFILE_DOMTERM=OFF -DQOBJFILE_WERROR=ON \
  -DYELLOWSTONE_ENABLE_ELF_BACKEND=ON \
  -DYELLOWSTONE_ENABLE_PE_BACKEND=ON -DYELLOWSTONE_ENABLE_MACHO_BACKEND=ON
cmake --build /tmp/opencode/qld-native-bridges --parallel 5
ctest --test-dir /tmp/opencode/qld-native-bridges --output-on-failure

cmake -S yellowstone -B /tmp/opencode/qld-native-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-fsanitize=address,undefined \
  -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON \
  -DQOBJFILE_DOMTERM=OFF -DQOBJFILE_WERROR=ON
cmake --build /tmp/opencode/qld-native-sanitize --parallel 5
env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/opencode/qld-native-sanitize --output-on-failure
```

| Configuration | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| GCC Debug, native defaults, warnings as errors | 18 | 0 | 1 |
| Clang Release, all explicit bridges ON, warnings as errors | 20 | 0 | 1 |
| Clang ASan/UBSan/leak detection, native defaults | 18 | 0 | 1 |

The existing `qbfd_source_notes` skip is due to its absent binutils source
snapshot. Logs: each build's `Testing/Temporary/LastTest.log`. Sanitizers cover
the linker/SDK, not the independently mapped freestanding machine code.

Installed strict C11 consumer (compiled, linked, run; resulting program exits 7):

```sh
cmake --install /tmp/opencode/qld-native-debug --prefix /tmp/opencode/qld-native-install
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I/tmp/opencode/qld-native-install/include/qobjld \
  -c yellowstone/ld/examples/native-sdk.c -o /tmp/opencode/qld-native-install/native-sdk.o
c++ /tmp/opencode/qld-native-install/native-sdk.o \
  -L/tmp/opencode/qld-native-install/lib \
  -lqld -llibstone_script_lua -ldl -lm -pthread \
  -o /tmp/opencode/qld-native-install/native-sdk
env PATH=/nonexistent QLD_TOOLCHAIN_BACKEND_ACTIVE=1 \
  /tmp/opencode/qld-native-install/native-sdk _start \
  /tmp/opencode/qld-native-install/program \
  /tmp/opencode/qld-native-debug/ld-tests/main.o \
  /tmp/opencode/qld-native-debug/ld-tests/add.o
/tmp/opencode/qld-native-install/program # expected exit 7
```

---

# Historical toolchain/script validation — 2026-11-17/18

The following report predates native-default selection. Its automatic bridge
selection and default build options have been superseded above.

Base LibStone revision: `803c23ca521d6c0ceb493450406fbcf185461a27`.
Local changes in `novostone/ActionSemantics` and `third_party/libuv` were present
before this work and were not changed by this linker pass.

## Implemented slice

- ELF now implements the shared `Backend.hpp` toolchain contract. Its duplicate
  POSIX workspace/process/response implementation was removed; native argument
  order, extension expansion and the C/C++ APIs remain intact.
- ELF checks output kind, explicitly static dependencies/interpreter, ignored
  options and image/map destination collisions before publishing results.
- Aurocks expression grammar and ASDL arena AST are generated during the build.
  The existing section/directive parser consumes the translated expression AST.
  Failed/speculative parsing is arena-owned; concurrent parser entry is
  serialized. The grammar was factored after the deep-invalid-input regression
  exposed excessive backtracking.
- MEMORY attributes select unassigned sections; REGION_ALIAS shares canonical
  cursors and ORIGIN/LENGTH queries. Section ALIGN, COMMON/NOLOAD placement,
  region overflow and input/output map reporting are exercised.
- Lua/Kaguya builders expose the same mapping operations with ordered pattern
  lists, alignment, NOLOAD, KEEP and sorting options.
- Native partial links preserve script-emitted bytes. Empty file-backed
  sections no longer mis-anchor BSS-only LOADs. Script-allocated COMMON symbol
  section identities are preserved. Section wildcard assembly respects token
  adjacency, including `?` and separate wildcard patterns.
- No public C option structure layout changes; no vendor changes.

## Environment and dependency identity

Linux `x86_64-pc-linux-gnu`; GCC 16.2.1, Clang 22.1.8,
GNU binutils/ld.bfd 2.47, Perl 5.42.2, Python 3.14.7.
Lua is bundled 5.5.0; Kaguya uses the existing compatibility header.
These dependencies are ordinary tracked source trees, not git submodules:

| Directory | Git tree at base revision |
| --- | --- |
| `third_party/lua` | `65275619e7829b569bfb18dea624358aa17dbc91` |
| `third_party/kaguya` | `fc814bd5d7f214240bd7abd03249dc4f87c48263` |
| `third_party/metatk` | `706f36de28c3d1ea5191a7493f6d5ed61fc3e7d4` |

Generator SHA-256:

```text
882d5c1ed69e46ef84488c86485bbec47f1237878d47c4bb58d1d039065f6396  scripts/aurocks.pl
1d04c59d3c8f90565eefa0d661a9d4650ac56e8968eedba3e12fd1fadab4d0c1  scripts/asdl.pl
```

## Commands and results

All commands run from the repository root. No root/superbuild or non-Linux host
build is claimed.

Baseline used the same Debug configuration with `QOBJFILE_WERROR=OFF`:
**17 passed, 1 skipped**. The initial strict build found three pre-existing
unused-function/parameter warnings in `position_dependenc.cpp` and `symbol.cpp`;
this pass removes the dead private helpers and marks the unused parameter.
The new wrong-ELF-kind test failed against the original backend (which accepted
an ET_REL object as successful shared output).

```sh
cmake -S yellowstone -B /tmp/opencode/qld-debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON \
  -DQOBJFILE_DOMTERM=OFF -DQOBJFILE_WERROR=ON
cmake --build /tmp/opencode/qld-debug --parallel 5
ctest --test-dir /tmp/opencode/qld-debug --output-on-failure

cmake -S yellowstone -B /tmp/opencode/qld-release \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON \
  -DQOBJFILE_DOMTERM=OFF -DQOBJFILE_WERROR=ON
cmake --build /tmp/opencode/qld-release --parallel 5
ctest --test-dir /tmp/opencode/qld-release --output-on-failure

cmake -S yellowstone -B /tmp/opencode/qld-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS=-fsanitize=address,undefined \
  -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON \
  -DQOBJFILE_DOMTERM=OFF -DQOBJFILE_WERROR=ON
cmake --build /tmp/opencode/qld-sanitize --parallel 5
env ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/opencode/qld-sanitize --output-on-failure

cmake -S yellowstone -B /tmp/opencode/qld-no-backends \
  -DCMAKE_BUILD_TYPE=Release \
  -DYELLOWSTONE_ENABLE_ELF_BACKEND=OFF \
  -DYELLOWSTONE_ENABLE_PE_BACKEND=OFF -DYELLOWSTONE_ENABLE_MACHO_BACKEND=OFF \
  -DYELLOWSTONE_BUILD_TESTS=ON -DYELLOWSTONE_BUILD_TOOLS=ON \
  -DQOBJFILE_DOMTERM=OFF -DQOBJFILE_WERROR=ON
cmake --build /tmp/opencode/qld-no-backends --parallel 5
ctest --test-dir /tmp/opencode/qld-no-backends --output-on-failure
```

| Configuration | Passed | Failed | Skipped |
| --- | ---: | ---: | ---: |
| GCC Debug, warnings as errors | 19 | 0 | 1 |
| Clang Release, warnings as errors | 19 | 0 | 1 |
| Clang ASan/UBSan + leak detection | 19 | 0 | 1 |
| GCC Release, all three toolchain backends OFF | 17 | 0 | 1 |

The skip is the existing `qbfd_source_notes` test: its local binutils source
snapshot is unavailable.
CTest logs are in each build's `Testing/Temporary/LastTest.log`. Sanitizers cover
the built linker, Lua runtime and SDK tests; freestanding assembly fixtures and
ordinary runtime-loader fixture DSOs are executed, not sanitizer-instrumented.

The retained dynamic regression executes compiler-driver links, symbol
versioning, TLS models/thread isolation, IFUNC, dlopen/dlclose, interposition,
PLT binding, copy relocations, archive groups, ordered as-needed and Lua
expansion. Existing PE/Mach-O tests cross-link and independently inspect
outputs; this is not Windows/dyld runtime validation.

`examples/memory.s` is assembled with each configured C compiler, linked with
both `memory.ld` and `memory.lua`, and executed (exit 42) by `tests/scripts.py`.

An installed C11 SDK consumer was also compiled, linked and executed:

```sh
cmake --install /tmp/opencode/qld-debug --prefix /tmp/opencode/qld-install
cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I/tmp/opencode/qld-install/include -c yellowstone/ld/examples/linker-sdk.c \
  -o /tmp/opencode/qld-install/example.o
c++ /tmp/opencode/qld-install/example.o -L/tmp/opencode/qld-install/lib \
  -lqld -llibstone_script_lua -ldl -lm -pthread -o /tmp/opencode/qld-install/example
/tmp/opencode/qld-install/example /tmp/opencode/qld-install/program
/tmp/opencode/qld-install/program # expected exit 7; observed 7
```

## Remaining limits

- This is not GNU ld parity or a new in-process dynamic-loader implementation.
  ELF dynamic functionality remains a GNU-compatible toolchain dependency.
- Only the expression part of the native script frontend is generated.
  PHDRS, AT/LMA, VERSION, INSERT and other documented native-subset limitations
  remain explicit diagnostics; GNU toolchain scripts have broader semantics.
- Forward aliases, file-selective/discarded COMMON allocation and lazy
  forward symbol/section evaluation are unsupported.
- The static emitter's mapping is still a small fixed ELF segment model.
  Memory attributes are selectors, not a protection/sandbox or general
  embedded ROM/RAM load-address model. Shared code/data pages may require RWX.
- The native partial linker is not a replacement for the toolchain's complete
  COMDAT/TLS/architecture relocation support.
- ELF kind checking distinguishes ET_EXEC/ET_DYN/ET_REL; it is not a separate
  full PIE-versus-DSO structural verifier.
- Auxiliary-file publication is not a multi-file atomic transaction.
- No Windows-host, Darwin-host, fuzzing campaign or full repository build was
  performed for this change set.
