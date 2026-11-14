# Compiler-validated discovery normalization cache

The subsequent [gate-A closeout](discovery-gate-a.md) adds compilation context and
preprocessing evidence, advances discovery to schema 2 / extractor `exoml-c11-3`,
and versions the internal cache wire to 2. The 32-test report below records the
preceding cache-only increment; the current ON suite has 33 tests.

## Design (P0.1 continuation)

The cache stores the existing owned `cdecl::TranslationUnit`, not a second
signature/contract model. Every request still parses with libclang, checks
compiler diagnostics and active implementation switches, and fingerprints the
actual parsed input buffers. Only declaration-graph normalization can be skipped.
This is deliberately **not a parse-free TU cache** and makes no speedup claim.

The content key combines the selected compiler/target/options, input/header and
provenance fingerprints, revision, extraction limits, schema/extractor version,
build-source identity, and a freshly serialized Clang AST. Serialization uses
Clang's timestamp-free mode. The AST digest accounts for preprocessing results
that a dependency list misses (new shadow headers, `__has_include`, conditional
fields, predefined/volatile macros). Native ASTs are temporary key material only:
no cached AST or other native object is ever loaded or executed.

The build identity hashes the extractor/codec/model/CLI sources and relevant
headers/template plus the host compiler/configuration. CMake reconfigures when
those files change. Updating model fields requires updating the shared codec
traversal/wire version and round-trip coverage, not adding a parallel database.

An internal bounded, versioned binary codec round-trips every model field and
validates enums, counts, graph references, provenance and the deterministic JSON
fingerprint before a hit. Cache artifacts are length-bounded and SHA-256 checked.
Malformed/truncated/stale entries are regenerated from the fresh compiler state,
never interpreted as safe contracts. Partial/error extractions are not cached.

Linux initially: cache directories must be user-owned and private (0700); files
are opened without following symlinks, written to private exclusive temporaries,
and atomically renamed. Concurrent writers may compute the same entry; readers
see a complete old/new artifact. Directory/storage failures fail the requested
extraction with diagnostics; corruption is explicitly reported as a rebuild.
Other platforms return a useful unsupported diagnostic if caching is requested;
uncached discovery is unchanged. A trusted same-user cache is assumed: checksums
detect corruption, not malicious re-authoring by that user. Cached discoveries
remain unsafe/unknown-effect and cannot authorize a safe export.

`ExtractOptions::cache_directory` opts in; a separate `CacheReport` exposes
disabled/miss/hit/rebuilt/bypassed/error, key and storage outcome without changing
deterministic reflection JSON. Existing one-argument `extract_c11` linkage stays.
`--cache-dir` and `--cache-report` expose this in the tool. Cache report output
never changes the declaration fingerprint. Header/input files must remain stable
during a request, as with uncached compiler discovery.

## Required tests

Cold/warm hits across independent processes; byte-identical normalized metadata;
header/provenance/define/undefine/revision/target/limit changes; include removal,
new shadow headers, negative `__has_include` dependencies and same-size/mtime
edits; malformed/truncated/oversized/version-mismatched artifacts; unsafe cache
permissions and symlinks; concurrent publishers; error results never cached;
real pinned vec/JSON/parsec hits; extraction OFF and installed package consumers.

## Verification report

### Outcome and remaining scope

Implemented the compiler-validated persistent normalization cache, bounded
lossless internal codec, source/build key identity, atomic Linux storage, SDK
activity reporting, CLI cache/budget options and regressions. The preceding
one-argument API and runtime dependency boundary remain intact. The discovery
JSON schema is still version 1; extractor identity is now `exoml-c11-2`.

New/changed implementation paths are `src/{Extract.cpp,ExtractCache.hpp,
ExtractCache.cpp,CacheCodec.cpp}`, `include/exoml/{Extract,CDecl}.hpp`,
`cmake/ExtractorBuild.hpp.in`, `CMakeLists.txt`, `tools/exoml-bind.cpp`,
`tests/{cache.py,cache_codec.cpp,consumer/}`, and ExoML/Moonstone documentation.
The prior discovery changes remain present; no vendor/MuLIB or analyzer source
was modified. Regression risks are internal cache-wire evolution, compiler
serialization behavior, and Linux filesystem semantics. Clang's own parser and
serializer costs are not bounded by exported-data budgets; neither timings nor
a parse-time speedup are claimed. Per-artifact limits are not a total cache
capacity/eviction policy.

Gate A is still partial: compile-commands ingestion, complete macro expansion
provenance/active-define snapshots and broader target ABI options remain open.
Parse-free TU reuse is not implemented. Semantic contracts, reviewed exports,
AOT adapters, cross-analysis bridges and On1x are unchanged/unimplemented.

### Reproducibility and baseline

At continuation start: LibStone HEAD
`a527489294547e3dcae75ed5f62b58650b6746af`, with the prior uncommitted discovery
slice intact; its existing Debug ON baseline passed **30/30** before this work.
Canonical MuLIB is still clean pinned commit
`f6f2eea234f1baa3fe73b8eb49194e7362b76278` at `/mnt/nvme/mulib`.
Header/manifest and dependency tree fingerprints are unchanged from
[the initial report](c11-discovery-report.md#revisions-and-dependencies).
The MuLIB extraction test independently checks source cleanliness and inventory;
no vendored MuLIB declarations are mixed in.

Environment: GCC/G++ 16.2.1 (`20260810`), Clang/Clang++/libclang 22.1.8,
OpenSSL Crypto 3.6.4, Python 3.14.7, native target
`x86_64-pc-linux-gnu`, Clang resources `/usr/lib/clang/22`. Cross-layout tests
remain compiler-only i386/x86-64 fixtures, not foreign-target executions.

All configurations retain ExoML, Chisel, tests and tools ON and Prolific OFF:

| Configuration | Result |
| --- | --- |
| GCC Debug, extraction ON | 32/32 |
| GCC Release, extraction ON | 32/32 |
| Clang Debug, ASan/UBSan, extraction ON | 32/32; no sanitizer failures |
| GCC Debug, extraction OFF | 28/28 |

Exact configure commands for the reused build directories are in the initial
report. They use `MOONSTONE_EXOML_EXTRACT=ON` and
`MOONSTONE_EXOML_MULIB_ROOT=/mnt/nvme/mulib` for the three ON configurations;
the Clang configuration sets both C/C++ flags to
`-fsanitize=address,undefined -fno-omit-frame-pointer`. Executed build/test commands:

```sh
cmake --build /tmp/opencode/exoml-discovery-debug --parallel 4
ctest --test-dir /tmp/opencode/exoml-discovery-debug --output-on-failure \
  --output-log /tmp/opencode/exoml-cache-debug-ctest.log
cmake --build /tmp/opencode/exoml-discovery-release --parallel 4
ctest --test-dir /tmp/opencode/exoml-discovery-release --output-on-failure \
  --output-log /tmp/opencode/exoml-cache-release-ctest.log
cmake --build /tmp/opencode/exoml-discovery-clang-san --parallel 4
ctest --test-dir /tmp/opencode/exoml-discovery-clang-san --output-on-failure \
  --output-log /tmp/opencode/exoml-cache-clang-san-ctest.log
cmake --build build-moonstone --parallel 4
ctest --test-dir build-moonstone --output-on-failure \
  --output-log /tmp/opencode/exoml-cache-off-ctest.log
```

Build output is retained beside those logs as
`/tmp/opencode/exoml-cache-{debug,release,clang-san,off}-build.log`.
The public headers and new targets build with warnings as errors. Installed
consumers exercise both existing native binding behavior and the new two-argument
extraction API, including actual cold/warm cache calls. No assertions were
disabled in Release, and no failing test was excluded. Existing unrelated
baseline warnings are not suppressed.

Clang static analysis passed on the extraction driver, cache store and codec:

```sh
clang++ --analyze -std=c++20 -I moonstone/exoml/include -I moonstone \
  -I /tmp/opencode/exoml-discovery-debug/exoml/generated \
  moonstone/exoml/src/Extract.cpp -o /tmp/opencode/exoml-cache-extract-analysis.plist
clang++ --analyze -std=c++20 -I moonstone/exoml/include -I moonstone \
  -I /tmp/opencode/exoml-discovery-debug/exoml/generated \
  moonstone/exoml/src/ExtractCache.cpp -o /tmp/opencode/exoml-cache-store-analysis.plist
clang++ --analyze -std=c++20 -I moonstone/exoml/include -I moonstone \
  moonstone/exoml/src/CacheCodec.cpp -o /tmp/opencode/exoml-cache-codec-analysis.plist
```

Changed C++ files pass clang-format's dry run and `git diff --check`. The
non-Linux conditional storage branch was syntax-checked with `-U__linux__`;
that is **not** an executed Windows/BSD/platform validation. No full root build,
TSan/cache performance result or sanitizer-built libclang is claimed.

### Coverage and negative evidence

All **52 functions** remain discovered and inventoried: vec 7, JSON 20,
parsec 25, with zero omitted functions. Each real header is extracted in separate
processes for a miss followed by a hit with identical JSON. Raw generated
bindings and safely reviewed exports remain **0 / 0**. Pointers, borrowed results,
callbacks, owned resources and special aggregates still require future contracts
and verified adapters; caching confers no trust.

`exoml_cache` tests independent processes, uncached/cold/warm equivalence,
same-size/mtime source and provenance edits, defines/undefines, revision,
target widths and limits; new include shadowing, removed includes and negative
`__has_include` probes (including changed field layout); corrupt/truncated/
oversized/wrong-version and checksum-valid malformed entries; private roots,
symlinks with an untouched sentinel, and twelve concurrent publishers. Invalid
source and exhausted graph budgets never publish a successful artifact.
`exoml_cache_codec` round-trips the full typed model including diagnostic notes,
fixes and UINT64_MAX, and rejects invalid enums/counts/booleans, dangling type
references, wrong schema/fingerprint and trailing bytes. Its production key
function distinguishes tool identities, AST identities and budget changes.
