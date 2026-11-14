# Novostone implementation audit — 2026-11-14

Audited repository revision: `321bcc989ece9e08cc2ba318c21aa14d02d98a3c`.
The existing six libraries were functional; the baseline passed 27/27 CTests.
This patch completes a source-to-analysis path and repairs the issues below.

| Component | Delivered change | Regression evidence |
| --- | --- | --- |
| Datalog | Aurocks grammar, ASDL-owned syntax, typed elaboration, source diagnostics, bounded input, deterministic source round trips | Frontend suite: nonlinear/mutual recursion, negation, integer limits, binary symbols, 500 seeded mutations, concurrent parses, long flat lists, rejected unsafe/malformed rules |
| CCDB | Atomic typed batch fact import; invalid batches preserve transaction facts and dependencies | Integration suite: import, failure rollback, recursive deletion, snapshot isolation, checkpoint restore from normalized source |
| DatabaseBasedIR | Built-in schemas and rules moved to `Analysis.dl`, parsed once and embedded without runtime file access; invalid opcode classification and guarded core replacement rejected | Existing SSA/liveness/CFG suites plus integration suite; exact pre/post migration `Program::signature()` comparison |
| QueryBasedIR | Validate the entire input corpus before early mismatch return, so malformed later inputs cannot be hidden | Integration suite plus existing optimizer, SCCP, cycle and differential validation suites |
| SpeculativeIR | Public refinement verification rejects a baseline that already contains speculative metadata | Integration suite plus existing recovery/invalidation suites |
| CostCC | Retired model versions stay retired even without logged decisions; reject heuristic arithmetic overflow | Integration suite plus existing cost/persistence/uncertainty suites |

The CLI adds `datalog FILE RELATION` and `format-datalog FILE`, with bounded
file reads. CLI regression tests now require both exit status zero and expected
output; matching output alone previously masked process failures.

## Generators and dependency provenance

Both generators are the repository versions at the revision above, unmodified:

| Tool | Version | SHA-256 |
| --- | --- | --- |
| `scripts/aurocks.pl` | 2.0 | `882d5c1ed69e46ef84488c86485bbec47f1237878d47c4bb58d1d039065f6396` |
| `scripts/asdl.pl` | 2.0 | `1d04c59d3c8f90565eefa0d661a9d4650ac56e8968eedba3e12fd1fadab4d0c1` |

No third-party dependency checkout, download or modification is required.
Build-time requirements add Perl 5.14+ and a C11 compiler to CMake/C++20/Threads.
The installed package uses compiled parser code and embedded analysis source.
The generated C symbols are prefixed to coexist with other Aurocks parsers.

## Verification commands and results

GCC 16.2.1, Debug, C11/C++20, Linux: **30/30 CTests passed**.

```sh
cmake -S novostone -B /tmp/novostone-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/novostone-build -j 4
ctest --test-dir /tmp/novostone-build --output-on-failure -j 4
```

Clang 22.1.8, Debug, ASan + UBSan + leak detection: **30/30 CTests passed**.
The sandbox's ptrace restrictions prevented LeakSanitizer from running; the
suite was rerun outside that sandbox with leak detection still enabled.

```sh
cmake -S novostone -B /tmp/novostone-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build /tmp/novostone-sanitize -j 4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir /tmp/novostone-sanitize --output-on-failure -j 4
```

Static installed-package consumer: **1/1 passed**. Release shared-library build,
copied installation prefix, consumer: **1/1 passed**. The relocated CLI executes
the installed Datalog example. `tests/consumer` exercises all six libraries
through `find_package(Novostone)` without source-tree include paths. See README
for consumer build commands. No aggregate whole-repository build was run.

## Scope and remaining boundaries

This is an executable reference compiler substrate for the documented i32 SSA
dialect. It does not implement arbitrary compiler backends or all proposals in
the design dossier. Datalog remains function-free, with integer/symbol columns
and stratified negation; arithmetic functors, aggregates, nominal column types
and differential maintenance across revisions are unsupported. The source
frontend is serialized, and column labels/comments are not semantic metadata.
CCDB recomputes affected cached queries from primary facts after edits.

The core IR text format, checkpoints and cost-model format retain their existing
parsers and compatibility. Only Datalog source parsing uses the new generated
frontend. Calls, floating point, allocation, atomics, FFI and native codegen
remain outside the IR dialect. Speculation supports guarded parameters and one
recovery frame, not OSR or nested native frames. Cost predictions remain ranking
estimates, and finite-domain differential tests are not universal proofs.
