# ActionSemantics delivery — 2026-11-17

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
