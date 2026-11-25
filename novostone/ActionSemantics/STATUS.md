# ActionSemantics delivery — 2026-11-17

**Current status:** see the data and patterns checkpoint at the end. Earlier sections
record previous deliveries and do not describe the current feature boundary.

## Native Aurocks and LLSynth integration

The specification parser now uses native Aurocks and its public parse/error
entry points with `$OFFSET` source locations. The LLSynth facet uses the real
`acsem` generator and embeds its Python runtime or links a generated C
companion; no second action interpreter is used. The focused LLSynth
integration suite exercises the five Python, C, and C++ tool targets.
The Novostone CTest suite passed 49/49 in GCC Debug, GCC Release, and
Clang ASan/UBSan builds with the native generator.

Baseline LibStone revision: `55755485acaded7c4ed804c0946bc8c7363f6763`.
`novostone/ActionSemantics` was empty. Before implementation, the standalone
GCC Debug Novostone baseline passed **30/30** tests.

## Implemented

- Aurocks `.acsem` parser, ASDL-owned AST, generated postorder elaboration visitor.
- Immutable validated `Language`, source-correlated `ParseError`, `parse`,
  `target`, and `generate` public APIs; `Novostone::ActionSemantics` CMake target.
- Constructor/sort/field checking, productive syntax, complete rule coverage,
  yielder typing, deterministic lowering, bounded parsing, concurrent host use.
- Action machine with transients, lexical bindings, tagged cells, checked i32
  arithmetic, completion, failure, escape, trapping, deterministic fallback.
- Standalone C11, Python, Rust and Free Pascal synthesis; LaTeX syntax/equations.
- Installed `acsem` CLI, grammar/schema, examples, documentation and package target.
- Real multi-target compilation/execution tests and the existing installed
  consumer extended to link and exercise the new library alongside Datalog.

The implementation is a documented dialect, not complete Action Notation.
Arbitrary concrete syntax, unfolding, closures, nondeterminism, concurrency and
user-defined data algebras remain unsupported. Program input uses constructor
terms. Outputs are interpreters rather than optimizing program compilers.

## Dependency provenance

The repository generators are unmodified, version 2.0:

| Generator | SHA-256 |
| --- | --- |
| `scripts/aurocks.pl` | `882d5c1ed69e46ef84488c86485bbec47f1237878d47c4bb58d1d039065f6396` |
| `scripts/asdl.pl` | `1d04c59d3c8f90565eefa0d661a9d4650ac56e8968eedba3e12fd1fadab4d0c1` |

No third-party checkout, download or vendor change. Host build: CMake, C11,
C++20, Perl 5.14+, Threads. Test orchestration additionally uses Python when
tools are enabled. Generated programs use only their target standard libraries.

## Verification

Environment: Linux `x86_64-pc-linux-gnu`, GCC 16.2.1, Clang 22.1.8,
Python 3.14.7, Rust 1.97.1, Free Pascal 3.2.2, pdfTeX 1.40.26 (TeX Live 2024),
CMake 4.4.3, Perl 5.42.2. Host warnings are enabled. C target outputs additionally
use `-std=c11 -Wall -Wextra -Werror -pedantic`; Pascal outputs use
`-Cr -Co -Ci -Sa`; Rust uses `--edition=2021 -Dwarnings`.

| Configuration | Evidence |
| --- | --- |
| Original GCC Debug baseline | 30/30 CTests |
| GCC Debug, static | 36/36 CTests |
| GCC Release, shared | 36/36 CTests; final backend regressions rerun separately |
| Clang Debug, ASan/UBSan and leak detection | 36/36 CTests; final six ActionSemantics suites also pass after capacity-order regression fix |
| Installed static and shared CMake consumers | 1/1 each, with `-Wall -Wextra -Wpedantic -Werror` |
| Release with tools and tests OFF | All seven libraries compile; no Python discovery required |
| Relocated shared installation | CLI validates installed specification; generated standalone C example prints `completed 42` |
| Isolated installed public header | GCC and Clang C++20 syntax checks, strict warnings as errors |

The final LaTeX equation formatting and escaped-identifier fixture passed both
affected Debug suites (frontend and LaTeX). The CLI also preserves an existing
output file when specification validation fails.

Each executable backend compiles four genuinely different specifications:
expression/storage, multi-sort/microsemantics, nullary minimal, and resource
limits. The expression suite contains 119 semantic cases (80 generated against
an independent checked-arithmetic oracle), plus malformed terms. Further cases
cover tuple/map/cell/symbol/node/depth/step limits, fresh-cell identity, unbound
values, branch-specific handling, lexical shadowing, environment replacement,
binding collisions, and persistent stores through failure/escape. LaTeX compiles
to PDF with shell escape disabled. Tests use exit status and observations.

A new capacity-boundary regression initially failed for C and Pascal: an
oversized binding union could report exhaustion before discovering a duplicate
key, unlike Python/Rust. The fix checks binding compatibility first, and all
four backends pass the regression. It is retained in `tests/backends.py`.

### Reproduction commands

```sh
cmake -S novostone -B /tmp/opencode/action-baseline -DCMAKE_BUILD_TYPE=Debug \
  -DNOVOSTONE_BUILD_TESTS=ON -DNOVOSTONE_BUILD_TOOLS=ON
cmake --build /tmp/opencode/action-baseline --parallel 4
ctest --test-dir /tmp/opencode/action-baseline --output-on-failure --parallel 4

cmake -S novostone -B /tmp/opencode/action-debug -DCMAKE_BUILD_TYPE=Debug \
  -DNOVOSTONE_BUILD_TESTS=ON -DNOVOSTONE_BUILD_TOOLS=ON
cmake --build /tmp/opencode/action-debug --parallel 4
ctest --test-dir /tmp/opencode/action-debug --output-on-failure --parallel 4

cmake -S novostone -B /tmp/opencode/action-release -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
cmake --build /tmp/opencode/action-release --parallel 4
ctest --test-dir /tmp/opencode/action-release --output-on-failure --parallel 4

cmake -S novostone -B /tmp/opencode/action-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build /tmp/opencode/action-sanitize --parallel 4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/opencode/action-sanitize --output-on-failure --parallel 4
# Final runtime-only changes:
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/opencode/action-sanitize -R novostone-action --output-on-failure --parallel 4

cmake --install /tmp/opencode/action-debug --prefix /tmp/opencode/action-install-static
cmake -S novostone/tests/consumer -B /tmp/opencode/action-consumer-static \
  -DCMAKE_PREFIX_PATH=/tmp/opencode/action-install-static \
  '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -Werror'
cmake --build /tmp/opencode/action-consumer-static --parallel 4
ctest --test-dir /tmp/opencode/action-consumer-static --output-on-failure

cmake --install /tmp/opencode/action-release --prefix /tmp/opencode/action-install-shared
cmake -S novostone/tests/consumer -B /tmp/opencode/action-consumer-shared \
  -DCMAKE_PREFIX_PATH=/tmp/opencode/action-install-shared \
  '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -Werror'
cmake --build /tmp/opencode/action-consumer-shared --parallel 4
ctest --test-dir /tmp/opencode/action-consumer-shared --output-on-failure

cmake -S novostone -B /tmp/opencode/action-library-only -DCMAKE_BUILD_TYPE=Release \
  -DNOVOSTONE_BUILD_TOOLS=OFF -DNOVOSTONE_BUILD_TESTS=OFF
cmake --build /tmp/opencode/action-library-only --parallel 4
```

CTest/sanitizer logs are in the corresponding build's
`Testing/Temporary/LastTest.log`; generated target artifacts live under
`ActionSemantics/test-{c,python,rust,pascal,latex}`. No sanitizer findings or
current test blockers. No whole-repository aggregate build, Windows/MSVC,
cross-target execution, or full Action Notation conformance is claimed.

## Historical expansion checkpoint — Phase 1, 2026-11-17

The subsystem now synthesizes two dialects from one parsed specification:

- **Dialect 1 was byte-frozen at this historical checkpoint.** That artifact
  guarantee was replaced by the unified-runtime consolidation described below.
- **Dialect 2** (selected with `--dialect 2`, automatic once a specification
  uses dialect-2-only constructs in later phases) targets a new universal
  runtime: an explicit continuation-based **Action Machine**. Action frames are
  bounded-pool objects, results travel through a pending slot, `evaluate`
  tail-replaces frames, and actions never recurse on the host stack — the
  foundation for closures, continuations, suspension, and tasks.
- Values stay tagged; environments and transient tuples became heap objects
  managed by **tracing mark-sweep collection** (explicit worklist, roots =
  frames + pending + canonical empties). Cell storage remains monotonic with
  its own budget, preserving dialect-1 observable behavior. In-flight child
  results live in frame slots (`rstatus/rdata/renv`) so collection stays sound
  mid-dispatch.
- New IR with a variadic operand pool, budgets and policies; the extended
  lowering originally ran separately (now consolidated into Model.hpp and
  ActionSemantics.cpp; instruction budget 8192 for extended specifications).
- New runtime templates mirror one machine: free-list heap,
  mark-sweep GC, frame dispatch, same statuses and exit-code contract as
  dialect 1. Integers default to checked i32 (dialect-1 parity); `int_width`
  policy is wired for 64-bit later. LaTeX renders dialect-2 equations with an
  Action Machine execution-contract section.
- CLI: `acsem [--dialect 1|2] [--define KEY=VALUE]... --target ...`; budget
  overrides (steps, nodes, symbols, refs, depth, heap) are dialect-2 synthesis
  options and error out with `--dialect 1`.
- Tests for dialect routing, determinism, provenance stamps and budget overrides
  (now merged into tests/frontend.cpp) and a second full backend round
  running the **entire legacy corpus through the dialect-2 machine**.

Verification (GCC 16.2.1 Debug, Linux): **12/12** `novostone-action*` CTests —
the six legacy suites plus six extended suites (frontend, C, Python, Rust,
Pascal, LaTeX). The dialect-2 C backend was additionally compiled with
`-fsanitize=address,undefined` and clean under binding/cell/step exhaustion and
a GC-churn program (`Try(Twice^16(Empty()))`, many collection cycles).

Known divergences from dialect 1 (documented, corpus-compatible): dialect-2
step accounting ticks once per frame dispatch (combinators ≈3 ticks vs 1), and
`evaluate` consumes no frame depth (tail-replacement), so boundary-exact step
or depth arithmetic differs near budgets while every corpus observation
matches. Resource errors remain distinct from semantic failure on all
backends. Phases 2–6 (closures/continuations/effects, data/patterns/objects/
modules, concurrency, nondeterminism/transactions, libuv/concrete syntax/docs)
are planned and not yet implemented; no unimplemented construct is accepted by
the grammar yet.

## Functional/control checkpoint — before source consolidation

Repository revision on entry: `803c23ca521d6c0ceb493450406fbcf185461a27`.
The working tree already contained an unfinished dialect-2 machine, closure
grammar/lowering and four runtime templates; this work was preserved and
completed rather than discarded. The initial build failed (missing lowering
brace and invalid C pool references). After build repair, the baseline was
**37/42 passing**, with all legacy suites passing and five extended suites
failing. Additional regressions exposed inconsistent closure syntax, recursive
closures pointing at closure-creation instructions, uninitialized counter cells,
incorrect example transient routing, Rust warnings, unrooted GC intermediates,
and frame-local rather than function-wide tail replacement.

### Delivered

- Preserved Aurocks/ASDL frontend and existing `.acsem` compatibility path.
- Explicit-frame dialect-2 actions, immutable instruction/operand tables,
  lexical environments, tagged values and cycle-aware tracing heap.
- Closures/abstraction, enact/apply with multiple arguments, lexical capture,
  escaping/nested closures, higher-order composition and explicit currying.
- letrec with shadowing and mutual recursion; return and function-wide tailcall.
- One-shot abortive callcc/resume, including replay after the capture returns.
  Saved control is copied; mutable storage and resource counters are shared.
- Proper unfolding substitution preserving pending combinators, plus explicit
  tailunfold; integer if/while, function-scoped break/continue.
- Tagged cell `put`, preserving the legacy integer-only `store` behavior.
- Lazy synchronous generators with nested/recursive suspension, return values,
  next/close, terminal/failure/escape propagation and reentrancy rejection.
- Checked SDK/CLI budgets and source-correlated extended diagnostics.
- All four standalone executable backends and LaTeX equations/contract.
- Five executable extended specifications with constructor-term programs;
  maintenance, notation, semantics, runtime and coverage documentation.

### Verification

Linux x86_64; GCC 16.2.1 Debug and Release host builds, Clang 22.1.8
ASan/UBSan Debug host and generated C. Python 3.14.7, Rust 1.97.1, Free Pascal
3.2.2 and pdfTeX 1.40.26. All generators remain unmodified.

| Build directory under `/tmp/opencode/` | Final result |
| --- | --- |
| action-expansion-debug | 42/42 CTests passed |
| action-expansion-release | 42/42 CTests passed |
| action-expansion-sanitize | 42/42 CTests passed; leak detection enabled, no sanitizer findings |
| action-expansion-consumer | Installed CMake consumer: 1/1 passed, strict C++ warnings |
| action-expansion-library-only | Release libraries built with tools/tests OFF; no Python discovery |
| action-committed-2a0wuj8e/build | Fresh HEAD baseline: 36/36 passed; five legacy expression artifacts byte-identical |
| action-expansion-installed-examples | Five installed C11 examples compile and execute with documented outputs |

Isolated installed public header checks pass GCC and Clang C++20 with
`-Wall -Wextra -Wpedantic -Werror`. CLI negative/oversized/zero budget requests
return 2 without overwriting an existing output file.

Each dialect-2 executable backend runs 167 semantic/oracle cases plus malformed
input, separate specifications, resource boundaries, 100000-call tail recursion,
10000 suspension cycles and small-heap collection stress. LaTeX compiles the
extended specifications with shell escape disabled. C outputs use
`-std=c11 -Wall -Wextra -Werror -pedantic`; Rust uses `-Dwarnings`; Pascal uses
`-Cr -Co -Ci -Sa`. Logs: each build's `Testing/Temporary/LastTest.log`.

```sh
cmake -S novostone -B /tmp/opencode/action-expansion-debug \
  -DCMAKE_BUILD_TYPE=Debug -DNOVOSTONE_BUILD_TESTS=ON -DNOVOSTONE_BUILD_TOOLS=ON
cmake --build /tmp/opencode/action-expansion-debug --parallel 6
ctest --test-dir /tmp/opencode/action-expansion-debug --output-on-failure --parallel 6
# Repeat using action-expansion-release and -DCMAKE_BUILD_TYPE=Release.
cmake -S novostone -B /tmp/opencode/action-expansion-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build /tmp/opencode/action-expansion-sanitize --parallel 6
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/opencode/action-expansion-sanitize --output-on-failure --parallel 6
cmake --install /tmp/opencode/action-expansion-debug --prefix /tmp/opencode/action-expansion-install
cmake -S novostone/tests/consumer -B /tmp/opencode/action-expansion-consumer \
  -DCMAKE_PREFIX_PATH=/tmp/opencode/action-expansion-install \
  '-DCMAKE_CXX_FLAGS=-Wall -Wextra -Wpedantic -Werror'
cmake --build /tmp/opencode/action-expansion-consumer --parallel 4
ctest --test-dir /tmp/opencode/action-expansion-consumer --output-on-failure
cmake -S novostone -B /tmp/opencode/action-expansion-library-only \
  -DCMAKE_BUILD_TYPE=Release -DNOVOSTONE_BUILD_TESTS=OFF -DNOVOSTONE_BUILD_TOOLS=OFF
cmake --build /tmp/opencode/action-expansion-library-only --parallel 6
```

### Incomplete work / compatibility boundaries

This is **not completion of the universal expansion mission**. Foundation work
remains partial (value kinds and type system), and Phase 2 lacks multi-shot/
delimited continuations, algebraic effects and cleanup. Phases 3–6 are largely
unimplemented: ADTs/patterns, objects/modules, tasks/promises/channels/actors,
structured concurrency, genuine parallelism, search/transactions/reactivity,
host capabilities, libuv and concrete grammar synthesis. No TypeScript-like
example is claimed.

Callcc/resume combined with generators is explicitly rejected at synthesis;
cross-delimiter lifecycle semantics are not implemented. Generator close discards
saved frames, without finalizers or cleanup. There is no cancellation guarantee.
Cells remain monotonic; tracing collects heap objects, not cell slots. Heap
budgets count objects, not bytes. Arithmetic stays checked i32. Pure yielders and
constructor parsing remain bounded host-recursive.

Action-step/depth accounting differs from dialect 1 near exact resource
boundaries. No aggregate repository build, Windows execution, cross-target
execution, arbitrary OS allocation-failure recovery, or full Action Notation
conformance has been verified. `third_party/libuv/` was already untracked on
entry and was not modified or integrated. See BACKENDS.md for the coverage matrix.

## Current checkpoint — source consolidation

All seven numbered source files were merged into their original counterparts
and removed. There are no filenames containing `2` under ActionSemantics:

- The model and instruction types live only in `Model.hpp`, using named field
  records, one variadic operand representation and common budgets.
- All AST validation/lowering lives in `ActionSemantics.cpp`. There is one
  canonical model, not independent legacy/extended lowerings.
- `Generate.cpp` has one table emitter and one LaTeX renderer.
- Each `runtime.{c,py,rs,pas}.in` contains the shared explicit-frame machine.
- Both frontend suites live in `tests/frontend.cpp`, in one executable.
  CMake backend suites now use `novostone-action-extended-*` names and
  `test-extended-*` output directories.
- Runtime embedding, CMake dependencies, README, DESIGN, RUNTIME and AGENTS
  instructions no longer reference the removed source paths.

Public dialect selectors remain compatible. One restricts language constructs
and preserves legacy semantic step accounting and logical recursion limits;
Two enables extended constructs/accounting and budget overrides. Both use the
same runtime implementation. Generated source is intentionally no longer
byte-identical to the former recursive interpreter.

The pre-consolidation working-tree baseline passed 42/42. The merged build
passes **42/42** in GCC Debug, GCC Release, and Clang ASan/UBSan (including
generated C; leak detection enabled, no findings). New regression checks cover
legacy step/depth boundary behavior, canonical tables in both modes, and
tagged-store detection across rule order. An additional 113 differential C
observations, including resource boundaries and seeded random programs, match
the committed pre-expansion interpreter. The installed CMake consumer passes
1/1 with strict C++ warnings. The Release library-only build also passes with
tools/tests OFF and without Python discovery.

Builds and logs: `/tmp/opencode/action-merged-{debug,release,sanitize}`,
each with `Testing/Temporary/LastTest.log`. Commands:

```sh
cmake -S novostone -B /tmp/opencode/action-merged-debug \
  -DCMAKE_BUILD_TYPE=Debug -DNOVOSTONE_BUILD_TESTS=ON -DNOVOSTONE_BUILD_TOOLS=ON
cmake --build /tmp/opencode/action-merged-debug --parallel 6
ctest --test-dir /tmp/opencode/action-merged-debug --output-on-failure --parallel 6
# Repeat with action-merged-release and -DCMAKE_BUILD_TYPE=Release.
cmake -S novostone -B /tmp/opencode/action-merged-sanitize \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined' \
  -DNOVOSTONE_BUILD_TESTS=ON -DNOVOSTONE_BUILD_TOOLS=ON
cmake --build /tmp/opencode/action-merged-sanitize --parallel 6
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/opencode/action-merged-sanitize --output-on-failure --parallel 6
```

This is a consolidation, not new coverage of the unimplemented semantic phases
listed above. The optional libuv checkout remains untouched.

## Current checkpoint — algebraic data and action patterns

Starting revision: `1e01d0d53`. Its Novostone Debug baseline passed 42/42.
The unified Aurocks/ASDL frontend and Action Machine now support recursive
nominal `data`, checked `construct`, `deconstruct`, `arity`, `component`,
and separate `equal` and `identical` operations. Action `match` supports
nested constructor, integer, token, wildcard, capture, as-, or-, and guarded
patterns, with scoped captures, optional typed coverage, and conservative
unreachable-arm diagnostics. Variant fields are traced by all executable
runtimes. The LaTeX emitter includes data declarations, patterns, and equations.

`examples/pattern_matching.acsem` and its matching `.term` demonstrate a
recursive list sum (`completed 42`). Cross-target tests cover shadowing,
alternative rollback, escaped closures, aliases, cycles through cells,
garbage collection, malformed specifications, and heap/depth exhaustion.

All 42 Novostone CTests passed with GCC Debug, GCC Release, and Clang
ASan/UBSan with leak detection. This includes compiled C11, Rust and Pascal,
Python execution and LaTeX compilation. Builds used the existing Novostone
configuration with tools and tests enabled; logs are in
`/mnt/nvme/$USER-acsem-build/{debug,release,sanitize}/Testing/Temporary/LastTest.log`.
The build directories were placed on the NVMe volume because `/tmp` filled.

This checkpoint does not add match expressions within yielders, richer scalar
or built-in collection types, a general type system, objects, modules, cleanup
handlers, algebraic effects, tasks or concurrency. See [BACKENDS.md](BACKENDS.md).

## Feature synchronization and rewrite checkpoint — 2026-11-21

Starting revision: `248ad1777`. Before changes, a fresh GCC Debug build passed
all **49/49** Novostone CTests, including the four generated executable backends
and LaTeX. Build directories for this checkpoint are
`/mnt/nvme/acsem-rewrite-build/{debug,release,sanitize}`; test logs are under
`Testing/Temporary/LastTest.log` in each build directory.

Implementation and final verification are in progress; the baseline result
above does not establish coverage of the new facilities.

The reference `scripts/acsem/acsem.t` passes. Its combined
`prove acsem.t backends.t` invocation cannot complete in this environment:
the backend harness attempts an empty Java compiler command when `javac` is
unavailable, after eight passing backend subtests.

## Native Aurocks frontend bootstrap

ActionSemantics and Datalog use native Aurocks' public parser, error accessor
and `$OFFSET` action reference. ASDL generation and the runtime backends are
unchanged. A fresh standalone `novostone` CMake build can build the host
Aurocks tool and the `acsem` target without the removed Perl generator.

All 35 artifacts generated from the seven checked-in examples (C, Python,
Rust, Pascal and LaTeX) matched the previous parser's artifacts byte for byte.
An additional 350 seeded mutations of those inputs had identical acceptance
and, where accepted, identical emitted C.
