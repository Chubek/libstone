# Novostone implementation audit

## Expanded PCC verification — 2026-11-19

- Rewrite replay covers all built-in QueryBasedIR rules, including exact CFG
  and phi repair, same-block memory forwarding, rooted dead cycles, and
  inductive SCCP checks. Replay uses the recorded options and observes committed
  rewrites rather than probing with fresh defaults. Structural checking is
  part of the trusted verification path.
- [`PCC/Native.hpp`](PCC/Native.md) lowers and independently checks a
  straight-line i32 subset of Linux x86-64 SysV, including spills, stack frames,
  callee-save preservation and clobbers at calls to a checked local helper.
  The test assembles and executes the output on the target host; the proof
  covers the assembly model, not linked object bytes or arbitrary callees.
- [`Novostone::PCCPackage`](PCC/Package.md) is enabled with
  `NOVOSTONE_PCC_SIGNED_PACKAGES=ON`. It uses OpenSSL 3 Crypto for Ed25519
  signatures and re-proves the candidate with Satie. The verifier uses a
  configured signing public key and can serve a bounded request via an authenticated
  OpenSSH forced command. The old `pcc` report remains unsigned; native
  instructions and proof status claims are not accepted in the wire format.

With GCC 16.2.1, Satie required, and OpenSSL 3.6.4, the integrated Novostone
suite passed **50/50** tests. The installed static consumer passed **1/1**.
The OpenSSH loopback test accepted the authenticated valid package and
rejected wrong keys, tampered packages and malformed framing.
With Satie disabled and signed packages built, all **8/8** PCC-focused
CTest cases passed, and the loopback verifier rejected the otherwise valid
signed package.

```sh
cmake -S novostone -B build-pcc -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/your/satie-prefix \
  -DNOVOSTONE_PCC_REQUIRE_SATIE=ON -DNOVOSTONE_PCC_SIGNED_PACKAGES=ON
cmake --build build-pcc --parallel
ctest --test-dir build-pcc --output-on-failure
python3 novostone/tests/pcc-package-ssh.py build-pcc/novostone \
  novostone/examples/pcc.nir --sshd /path/to/sshd
```

## Proof-carrying optimization addition — 2026-11-18

Audited repository revision before this change:
`934b6ed029bfea28bbf23b5dc77c476154b0dd0d`. The existing Novostone baseline
passed 42/42 CTests.

Added the `Novostone::PCC` library, `novostone pcc` command, and versioned
artifact core. The implementation accepts verified i32 IR plus explicit rewrite
constraints, computes and ships a complete source SCCP analysis, chooses a
deterministic allowed optimizer prefix, and retains source and optimized IR with
full transformation records and work counters.

| Area | Implemented | Deliberate boundary |
| --- | --- | --- |
| Rewrite artifacts | Constraint allow-list, structural/work caps, deterministic prefix reconstruction, canonical core schema v1 | Stops at the first disallowed rewrite; no reordering around policy |
| SMT proof emission | Exact modular-i32 Boolean bit-blasting for constant folds; independently encoded local CSE; checked algebraic/copy/uniform-phi obligations | Branch folding, load forwarding, DCE/dead cycles, and conditional constants are rejected |
| Satie conduit | Explicit SMT-LIB2 frontend selection; syntax status; UNSAT proof, SAT counterexample, and Unknown results | Missing/unsupported solver support never becomes a positive result |
| Register allocation | Liveness interference, deterministic register-only coloring, argument/return and explicit constraints, one-hot Boolean obligation | No spills, stack ABI, calls/clobbers, machine constraints, or prologue/epilogue; callee-saved use is Unknown |
| CLI/package | Source and optimized IR, canonical artifact, constraints, analysis, transformations, and length-prefixed SMT code; `--require-proof` | Emission only: no package parser, signature, remote verifier, or proof cache |
| Packaging | Installed `Novostone::PCC`, public headers and manual; optional Satie dependency propagated for static consumers | Shared builds absorb private Satie; no solver is required when configured off |

The authoritative implementation paths are `PCC/PCC.{hpp,cpp}`,
`PCC/SMTProof.{hpp,cpp}`, and
`PCC/RegisterAllocation.{hpp,cpp}`. The supported API and trust boundary are
documented in [PCC/README.md](PCC/README.md). `tests/pcc.cpp` includes positive
and negative rewrite, solver, replay, injection, resource-bound, interference,
and calling-convention cases. `tests/consumer` now exercises PCC through the
installed CMake package.

The vendored Satie CMake files were repaired only to make absent optional D
frontend and QaMRpp files explicit: default-disabled functionality is skipped,
while requesting `BUILD_D_FRONTENDS=ON` without its complete source set fails
configuration. Satie remains a separately compiled optional conduit.

### PCC verification matrix

All runs were on Linux with `TMPDIR=/mnt/nvme/.opencode-tmp-pcc`.

| Configuration | Result |
| --- | --- |
| GCC 16.2.1, Debug, Satie required/enabled | **48/48 CTests passed** |
| GCC 16.2.1, Debug, Satie disabled | **48/48 CTests passed**; proof statuses remain `Unknown` and `--require-proof` exits 3 |
| Clang 22.1.8, Debug, ASan + UBSan + leak detection, Satie enabled | **48/48 CTests passed** |
| GCC 16.2.1, Release, shared libraries, Satie enabled | **48/48 CTests passed** |
| Vendored Satie, GCC Release | **2/2 available core CTests passed** |
| Static installed package with Satie, installed consumer | **1/1 passed** |
| Static installed package without Satie, installed consumer | **1/1 passed** |
| Copied/relocated Release shared package, no Satie prefix supplied to consumer | **1/1 passed**; installed CLI emitted and proved the PCC example |
| Scoped Libstone aggregate configure (`Limestone` + `Novostone`) | `novostone_PCC` built; Limestone's header/runtime-only Satie target was correctly treated as unavailable for proof discharge |
| Negative CMake configurations | Contradictory `USE=OFF,REQUIRE=ON` and required-but-undiscoverable Satie both failed configuration with actionable diagnostics |

Representative commands:

```sh
cmake -S third_party/satie -B /mnt/nvme/.opencode-build-satie-pcc-final \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/mnt/nvme/.opencode-satie-pcc-final-install \
  -DBUILD_TESTING=ON -DBUILD_EXAMPLES=OFF -DINSTALL_CLI=OFF \
  -DBUILD_D_FRONTENDS=OFF -DBUILD_STDPLUGIN=OFF
cmake --build /mnt/nvme/.opencode-build-satie-pcc-final --parallel 8
ctest --test-dir /mnt/nvme/.opencode-build-satie-pcc-final --output-on-failure
cmake --install /mnt/nvme/.opencode-build-satie-pcc-final

cmake -S novostone -B /mnt/nvme/.opencode-build-pcc-gcc-debug-final \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/mnt/nvme/.opencode-satie-pcc-final-install \
  -DNOVOSTONE_PCC_USE_SATIE=ON -DNOVOSTONE_PCC_REQUIRE_SATIE=ON
cmake --build /mnt/nvme/.opencode-build-pcc-gcc-debug-final --parallel 8
ctest --test-dir /mnt/nvme/.opencode-build-pcc-gcc-debug-final \
  --output-on-failure

cmake -S novostone -B /mnt/nvme/.opencode-build-pcc-gcc-debug-off \
  -DCMAKE_BUILD_TYPE=Debug -DNOVOSTONE_PCC_USE_SATIE=OFF
cmake --build /mnt/nvme/.opencode-build-pcc-gcc-debug-off --parallel 8
ctest --test-dir /mnt/nvme/.opencode-build-pcc-gcc-debug-off \
  --output-on-failure

CC=clang CXX=clang++ cmake -S novostone \
  -B /mnt/nvme/.opencode-build-pcc-clang-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/mnt/nvme/.opencode-satie-pcc-final-install \
  -DNOVOSTONE_PCC_REQUIRE_SATIE=ON \
  '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build /mnt/nvme/.opencode-build-pcc-clang-sanitize --parallel 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir /mnt/nvme/.opencode-build-pcc-clang-sanitize \
  --output-on-failure

cmake -S novostone -B /mnt/nvme/.opencode-build-pcc-gcc-release-shared \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_PREFIX_PATH=/mnt/nvme/.opencode-satie-pcc-final-install \
  -DNOVOSTONE_PCC_REQUIRE_SATIE=ON
cmake --build /mnt/nvme/.opencode-build-pcc-gcc-release-shared --parallel 8
ctest --test-dir /mnt/nvme/.opencode-build-pcc-gcc-release-shared \
  --output-on-failure

cmake --install /mnt/nvme/.opencode-build-pcc-gcc-debug-final \
  --prefix /mnt/nvme/.opencode-install-pcc-static
cmake -S novostone/tests/consumer \
  -B /mnt/nvme/.opencode-build-pcc-consumer-static \
  '-DCMAKE_PREFIX_PATH=/mnt/nvme/.opencode-install-pcc-static;/mnt/nvme/.opencode-satie-pcc-final-install'
cmake --build /mnt/nvme/.opencode-build-pcc-consumer-static
ctest --test-dir /mnt/nvme/.opencode-build-pcc-consumer-static \
  --output-on-failure

cmake --install /mnt/nvme/.opencode-build-pcc-gcc-debug-off \
  --prefix /mnt/nvme/.opencode-install-pcc-off
cmake -S novostone/tests/consumer \
  -B /mnt/nvme/.opencode-build-pcc-consumer-off \
  -DCMAKE_PREFIX_PATH=/mnt/nvme/.opencode-install-pcc-off
cmake --build /mnt/nvme/.opencode-build-pcc-consumer-off
ctest --test-dir /mnt/nvme/.opencode-build-pcc-consumer-off \
  --output-on-failure

cmake --install /mnt/nvme/.opencode-build-pcc-gcc-release-shared \
  --prefix /mnt/nvme/.opencode-install-pcc-shared
cp -a /mnt/nvme/.opencode-install-pcc-shared \
  /mnt/nvme/.opencode-relocated-pcc-shared-20261118
cmake -S novostone/tests/consumer \
  -B /mnt/nvme/.opencode-build-pcc-consumer-shared-relocated \
  -DCMAKE_PREFIX_PATH=/mnt/nvme/.opencode-relocated-pcc-shared-20261118
cmake --build /mnt/nvme/.opencode-build-pcc-consumer-shared-relocated
ctest --test-dir /mnt/nvme/.opencode-build-pcc-consumer-shared-relocated \
  --output-on-failure
```

The partial vendored Satie snapshot has no QaMRpp subtree, so its
plugin/Lua-stdlib aggregate test is explicitly not registered; it is not counted
as passed. `BUILD_D_FRONTENDS=OFF` was explicit, and no D test is claimed. No
whole-repository aggregate build was run for this PCC change; the scoped
aggregate smoke disabled unrelated components and tests.

## ActionSemantics addition — 2026-11-17

Added the independent `Novostone::ActionSemantics` library and `acsem` CLI.
The `.acsem` frontend uses the repository Aurocks parser generator and ASDL AST
and visitor generator. It synthesizes standalone C11, Python, Rust and Free Pascal
interpreters plus compilable LaTeX documentation. The dialect covers transient
data, scoped bindings, tagged storage cells, checked arithmetic, failure and escape.
See [ActionSemantics/STATUS.md](ActionSemantics/STATUS.md) for evidence and
[ActionSemantics/README.md](ActionSemantics/README.md) for its exact supported scope.

## Previous compiler-method audit — 2026-11-14

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
the installed Datalog example. `tests/consumer` now exercises all eight
installed libraries through `find_package(Novostone)` without source-tree
include paths. See README for consumer build commands. No aggregate
whole-repository build was run.

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
