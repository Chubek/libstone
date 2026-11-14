# P0.1 / gate A closeout design

## Invariants and implementation scope

Finish the compiler-driven discovery path, not semantic contracts or native
adapter generation. All declarations remain discovered, unsafe and unknown-effect.
Preserve the existing interpreter and one-/two-argument extraction APIs.

Compilation databases are read as bounded strict JSON using the existing vendored
nlohmann JSON implementation; no database compiler, wrapper, shell or response
file is executed. Select an exact source entry (or the sole entry); ambiguous
entries fail. Resolve relative paths against the recorded working directory
without changing process-global cwd. Preserve original arguments, selected
driver/directory/file, stripped output/dependency actions, normalized compiler
options and database fingerprints. Reject unknown flags, plugins, response files,
non-C11 language modes and shell expansion rather than silently dropping ABI input.
Explicit developer options override the selected command in documented order.

Macro evidence includes compiler-reported definitions and source expansions,
include edges and skipped source ranges. A second read-only compiler parse appends
uniquely named `#ifdef` probes to the original main-file buffer to obtain the
final defined/undefined state for observed names and standard dynamic builtins.
Probe code never expands arbitrary macro replacements or executes native code.
Active-definition IDs are recorded only when libclang supplies them; restored or
dynamic definitions without such a reference remain explicitly unknown. Nested
replacement-token expansion trees are not exposed by libclang and are not
fabricated. Probe failures remain visible diagnostics, not false complete evidence.

Discovery JSON becomes schema 2 (still unrelated to semantic-contract v2), with
new fields added to the owned typed IR. Existing fields/interfaces are preserved;
the internal cache wire format/source identity is bumped and round-trip tested.
Both primary and probe input buffers must agree; changing files during discovery
fails closed. Fingerprints include compiler options, compile database, provenance,
source inputs and compiler-reported preprocessing evidence. Native cache keys
still use a fresh compiler AST and never load native cached data.

Gate evidence must cover actual pinned vec/JSON/parsec function **and type**
inventories, conditional/macro/callback/array/aggregate fixtures, altered ABI and
target options, rejected dangerous input, deterministic cache invalidation,
installed SDK consumers, Debug/Release, GCC/Clang and sanitizer/OFF builds.
Compiler-specific/unsupported constructs must remain explicit. A tested initial
gate is not a claim of all-platform/all-C-extension or full-preprocessor support.

## Supported compilation context

Database entries require `directory`, `file`, and exactly one of `arguments`
(string array) or `command` (restricted POSIX quoting); optional `output` is
accepted as metadata. Duplicate/unknown keys, malformed types, excessive depth,
byte/entry budgets and ambiguous selections fail. `--compile-file` selects the
exact source; otherwise an exact header entry or sole entry is required. Non-C
source suffixes need an explicit `-x c`/`c-header`. The recorded driver is not
executed and does not establish GCC/MSVC equivalence with libclang.

Paths are resolved relative to the entry directory. Supported parser inputs:

- C11/GNU11/ISO-2011 modes, `-x c`/`c-header`.
- Include, system, quote and after-include directories; forced includes and
  macro files; sysroot and resource directory; object-like defines/undefines.
- Explicit target, `-m32`/`-m64`/`-mx32`, target ABI/CPU/architecture/tuning,
  short enum/wchar, char signedness, packing (1/2/4/8/16), double alignment,
  MS bitfield/extension and float ABI options.
- Thread, PIC/PIE, bounded named optimization/debug modes, warning/pedantic,
  no-builtin and no-standard-include flags recognized by the implementation.

Database compile/output/dependency actions (`-c`, `-S`, `-fsyntax-only`, `-o`,
`-MD`/`-MMD`/`-MP`, `-MF`/`-MT`/`-MQ`) are stripped and recorded, never run.
Driver actions are not accepted as direct `--compile-arg` inputs. Plugins,
response files, arbitrary `-Xclang`/`-Xpreprocessor`, module/PCH-loading flags,
shell expansion/operators, other language standards and all unclassified flags
are rejected with useful diagnostics. There is no presence-only success fallback.
This bounded option vocabulary is intentional; it is not a general compiler driver.

## Verification report

### Outcome

**Initial gate A passed for the pinned three-header Linux profile.** Real compiler
declarations, typed source/ABI context, provenance, deterministic metadata and
cache invalidation are implemented, packaged and tested. There are no invented
signatures or dropped functions/types in that profile. This closes the current
P0.1 milestone, not the entire ExoML/MuLIB/On1x mission.

New public fields: `CompilationContext`, `Preprocessing`, macro definition/
expansion/state and include-edge records; `Parameter::tokens`; and
`ExtractOptions::{compile_commands,compile_file,compiler_arguments}`. Prior
extraction overloads and legacy static/interpreter binding interfaces remain.
Discovery JSON is schema **2**, extractor `exoml-c11-3`; cache wire is **2**.
Schema-1 discovery artifacts are not silently upgraded or treated as reviewed
contracts. The new fields round-trip through the same internal typed cache codec.

Actual closeout paths: `src/{CompileOptions.hpp,CompileOptions.cpp,Extract.cpp,
CDecl.cpp,CacheCodec.cpp,ExtractCache.cpp}`, `include/exoml/{CDecl,Extract}.hpp`,
`CMakeLists.txt`, `tools/exoml-bind.cpp`,
`tests/{discovery_context.py,discovery.py,mulib-discovery-inventory.json,
cache.py,cache_codec.cpp}`, the JSON license notice, local `AGENTS.md` forwarding
entry and README/design/status/manual updates. Previous discovery/cache edits
are retained. No MuLIB API, vendor source, Git remote or unrelated implementation
was modified.

Regression risks are explicit: additive metadata/schema changes, compiler flag
coverage, provenance payload size and the second compiler pass. Macro snapshot
coverage means compiler-reported source events plus observed/CLI and standard
dynamic builtin names; it is not all possible vendor builtin spellings. Libclang
does not provide nested replacement-token trees or every restored/dynamic active
definition ID. Missing IDs carry reasons; a known defined state is not confused
with known replacement semantics. Poisoned/unsupported probes fail with partial
graph/diagnostics. Arbitrary extensions/flags and untested platforms stay outside
the verified profile. Files must remain stable; reread fingerprints and probe
include/conditional comparisons diagnose observed drift, not an atomic filesystem
snapshot guarantee.

### Revisions and dependencies

Baseline at this closeout: LibStone HEAD
`a527489294547e3dcae75ed5f62b58650b6746af`, with prior discovery/cache working
changes intact; **32/32** Debug ON tests passed before edits. Canonical MuLIB
remains clean at `f6f2eea234f1baa3fe73b8eb49194e7362b76278`, separate from the
unused vendored snapshot. Header/manifest fingerprints and original vendor tree
IDs remain those recorded in [the initial report](c11-discovery-report.md).

The added developer-only JSON dependency reuses existing nlohmann **3.10.5**:

```text
third_party/metatk/third_party/tomlplusplus/vendor/json.hpp
SHA256 2bef58693f88d9f6391b9a858ee003fe0769193940e03fd3051abd8670d97658
```

Its source is unmodified, build identity includes its hash, and its MIT notice is
installed with the optional extraction package. Ordinary runtime/static binding
consumers need neither this header nor libclang/OpenSSL. No downloads occurred.

Native environment: GCC/G++ **16.2.1** (`20260810`), Clang/Clang++/libclang
**22.1.8**, OpenSSL **3.6.4**, Python **3.14.7**, target
`x86_64-pc-linux-gnu`, resources `/usr/lib/clang/22`. i386/x86-64 cross-layout
fixtures are compiler-only; no foreign-target execution is claimed.

### Coverage

| Pinned public header | Functions | Typedef declarations | Record declarations | Enum declarations | Generated raw / reviewed-safe bindings |
| --- | ---: | ---: | ---: | ---: | --- |
| mulib-vec.h | 7 | 1 | 1 | 0 | 0 / 0 |
| mulib-json.h | 20 | 3 | 2 | 1 | 0 / 0 |
| mulib-parsec.h | 25 | 11 | 8 | 3 | 0 / 0 |

All **52 functions and 30 type declarations** match source-derived inventories;
parsec's forward declaration and later definition are deliberately both counted.
Function lists also match canonical MuLIB `Inventory.json`. Header hashes are
checked against `tests/mulib-discovery-inventory.json`; **zero public function/
type declarations are omitted** for these pinned headers. Dependency headers are
not separately exported as public modules, but referenced types, fingerprints,
macro/include provenance remain available. Header guards/macros have no callable
C ABI. Unsupported constructs elsewhere retain spelling, source and an actionable
reason rather than a fabricated type.

No discovered pointer, callback, resource or aggregate is promoted to a safe API.
Contracts, ownership verification, direct AOT adapters and On1x export remain
P0.2/P0.3/later work. These tests do not execute MuLIB kernels or establish their
ownership behavior; that is a later acceptance gate.

### Tests and commands

| Configuration (Prolific OFF, ExoML/Chisel/tests/tools ON) | Result |
| --- | --- |
| GCC Debug, extraction ON | 33/33 |
| GCC Release, extraction ON | 33/33 |
| Clang Debug, ASan/UBSan, extraction ON | 33/33, no sanitizer failures |
| GCC Debug, extraction OFF | 28/28 |

The existing configured directories use the exact options in the earlier
reports. Debug additionally enables `CMAKE_EXPORT_COMPILE_COMMANDS=ON` for an
actual CMake database example. Executed final build/test commands:

```sh
cmake --build /tmp/opencode/exoml-discovery-debug --parallel 4
ctest --test-dir /tmp/opencode/exoml-discovery-debug --output-on-failure \
  --output-log /tmp/opencode/exoml-gate-a-debug-ctest.log
cmake --build /tmp/opencode/exoml-discovery-release --parallel 4
ctest --test-dir /tmp/opencode/exoml-discovery-release --output-on-failure \
  --output-log /tmp/opencode/exoml-gate-a-release-ctest.log
cmake --build /tmp/opencode/exoml-discovery-clang-san --parallel 4
ctest --test-dir /tmp/opencode/exoml-discovery-clang-san --output-on-failure \
  --output-log /tmp/opencode/exoml-gate-a-clang-san-ctest.log
cmake --build build-moonstone --parallel 4
ctest --test-dir build-moonstone --output-on-failure \
  --output-log /tmp/opencode/exoml-gate-a-off-ctest.log
```

Build logs are `/tmp/opencode/exoml-gate-a-{debug,release,clang-san,off}-build.log`.
All public headers and changed targets compile with warnings as errors. Existing
unrelated baseline warnings are not suppressed; Release assertions stay enabled.
Installed runtime/extractor consumers link and execute. The sanitizer run covers
our code, not a sanitizer-built libclang or native MuLIB ownership driver.

New context regressions verify strict/ambiguous/duplicate-key/malformed database
handling, no driver or shell execution, no requested object/dependency file
creation, relative include directories and explicit overrides, dangerous flags,
macro undef/redefine/push-pop/dynamic states, include and skipped-range evidence,
raw source tokens, type-ID ABI scoping, and database/ABI cache invalidation.
Native C11 probes built with both selected host compilers independently compare
enum width, record alignment and char signedness under short-enum, packing and
signedness flags. Cross-width fixtures verify long/pointer widths without a
target runner. Previous parser/static binding/ABI rejection/cache regressions
remain enabled.

Clang static-analyzer checks also passed on `CompileOptions.cpp`, `Extract.cpp`,
`CDecl.cpp` and `CacheCodec.cpp`; no static-analysis result is treated as a safety
proof. Commands used `clang++ --analyze -std=c++20 -I moonstone/exoml/include
-I moonstone`, with the existing JSON vendor directory supplied as a system
include for `CompileOptions.cpp`. Analyzer logs are
`/tmp/opencode/exoml-gate-a-{options,extract,cdecl,codec}-analysis.plist`.
Changed C++ files pass clang-format's dry run, and `git diff --check` is clean.

The README database example was executed against the real CMake-generated DB:

```sh
/tmp/opencode/exoml-discovery-debug/exoml/exoml-bind extract \
  --header moonstone/exoml/tests/fixture.h \
  --compile-commands /tmp/opencode/exoml-discovery-debug/compile_commands.json \
  --compile-file moonstone/exoml/tests/fixture.c \
  --resource-dir /usr/lib/clang/22 --out /tmp/opencode/exoml-gate-a-example.cdecl.json
```

No complete LibStone root build, Windows/MSVC/BSD validation, all-target support,
parse-time speedup, semantic-contract proof or On1x compilation is claimed.
