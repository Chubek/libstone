# 9. Diagnostics, testing and limitations

## Diagnostic families

| Prefix | Typical failure |
| --- | --- |
| `parse` / `parse.limit` | Syntax, literal decoding, token/byte/type-depth budget. |
| `type.*` | Unknown/cyclic parameters, invalid value type or opaque type contract. |
| `dialect.*` / `operation.*` | Missing registration, bad parameter variant or arity. |
| `cfg.*` / `ssa.*` | Invalid terminator, unreachable block, missing predecessor, duplicate definition or dominance failure. |
| `call.*` / `return.*` | Signature disagreement. |
| `literal.*` | Invalid Boolean, finite float or signed bit-pattern range. |
| `lowering.*` | Missing rule, illegal direction, lost result identity or rewrite budget. |
| `analysis.*` / `pass.*` | Missing analysis, failed verification or change-contract mismatch. |
| `vm.*` | Unsupported execution type/target, bad signature, runtime trap or budget exhaustion. |
| `sandstone.*` / `native.*` | Missing target facts, inconsistent handoff, illegal form/binding, unresolved spill or bad relocation. |
| `backend.unavailable` | Explicitly requested backend has not been registered. |

`Error::diagnostics()` exposes structured C++ diagnostics. `format` includes
available source/function/block/operation context. The C ABI retains the first
error as borrowed thread-local text. Callback exceptions are converted at
verification/pass/C boundaries; do not use callbacks as an exception mechanism
across a foreign native ABI.

## Default bounds

- Input source: 16 MiB.
- Lexed tokens: one million.
- Type nesting/substitution: 64 levels.
- Total specialized type expansion: one million visited nodes, including aliases.
- Module functions: 100000.
- Per function: 10000 blocks, one million operations.
- Conversion: 100000 rewrite applications by default.
- Convenience execution: one million bytecode instructions by default.
- Native encoded image: 64 MiB.

`ParseLimits`, the conversion rewrite argument and the execution budget expose
the principal configurable bounds. These limits are not a sandbox for custom
C++/Lua code, operating-system timeouts, or a guarantee of constant-time graph
algorithms. Firestone and Limestone also enforce their own verifier/scheduler
resource contracts.

## Run tests

```sh
cmake -S flintstone -B /tmp/opencode/flintstone-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/opencode/flintstone-build --parallel 2
ctest --test-dir /tmp/opencode/flintstone-build --output-on-failure
```

The suites cover:

| Suite | Evidence |
| --- | --- |
| `flintstone.core` | High/core/VM conversion; parse/print round trips; type and dialect specialization; loops/phis/dominance; forward/mutual recursion; native VM callbacks; heap-cell memory; modular overflow; vector/tuple scalarization (including loop-carried aggregates); math extensions; trap execution; ptr/atomic verification and explicit VM rejection; Blackstone/DCE; transactional failure and extension APIs. |
| `flintstone.native` | Bank-backed instruction forms; model/resource import; scheduling and liveness; all four allocators (linear-scan, greedy, coloring, PBQP); aliases/ties/fixed registers; exact encoding; relocation rebasing; negative handoffs; shipped x86-64/AArch64/RISC-V tiny targets checked byte-exactly with host execution on AMD64; 27-architecture registry. |
| `flintstone.c` | Separate C translation units, compatibility header, lifecycle, integer execution, diagnostics, invalid arguments and context-independent module ownership. |
| `flintstone.cli` | Real example execution, conversion, bytecode framing and error exit behavior. |
| `flintstone.xbind` | Executed manifest validation and generated exact C signature assertions. |
| `flintstone.lua` | Generated Lua glue and real DomLuaBind API compiled/executed with lifecycle and conversion calls. |
| `flintstone.install` | Installed prefix moved, package rediscovered, separate C/C++ consumer compiled and executed. |

Binding tests require Python for the existing DomLuaBind generator. Disable
`FLINTSTONE_BUILD_BINDING_TESTS` explicitly if that toolchain is intentionally
unavailable; do not describe those tests as passing when disabled.

### Sanitizers

```sh
cmake -S flintstone -B /tmp/opencode/flintstone-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_SHARED_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build /tmp/opencode/flintstone-sanitize --parallel 2
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir /tmp/opencode/flintstone-sanitize \
  --output-on-failure
```

The generated native-code execution test disables only Clang's function-type
metadata check at that handoff: dynamically emitted code has no Clang metadata.
Address/undefined checks on the IL, adapters and runtime remain enabled.
The installation test propagates compiler/sanitizer flags to its consumer.

Some ptrace-based runners prevent LeakSanitizer from starting; in that specific
environment use `ASAN_OPTIONS=detect_leaks=0` and report leak checking as
unavailable, not as a passing leak test.

## Current boundaries

This implementation supplies a working scalar/SSA/VM vertical pipeline and a
verified native backend handoff. It is not a universal production backend for
every ISA in the Stone metadata collections.

- No LLVM/MLIR/QBE dependency or second SAT/SMT framework is introduced.
- No automatic generic monomorphization across multiple runtime type arguments,
  nested region representation, or exception-unwinding IL is supplied.
- Vector packs and tuple aggregates are eliminated by explicit scalarization
  passes (`vector_scalarize_pass`, `tuple_sroa_pass`, or `--scalarize`); values
  that would cross function boundaries fail instead of changing signatures.
- The IL represents multi-result/opaque constructs, but Firestone scalar
  execution requires explicit lowering to supported scalar types. Pointer
  arithmetic and atomic ordering have no portable VM lowering and require a
  native target adapter.
- Builtin integer overflow semantics are modular; unsupported policies are
  rejected. Floating operations have Firestone's ordinary binary64 semantics,
  not an implicit fast-math algebra.
- Portable memory is VM heap-cell storage, not a native atomic/volatile memory
  model. Use a dedicated dialect/backend for richer memory contracts.
- Native selection, complete ABI lowering, phi destruction, prologue/epilogue,
  spill materialization, delay slots, object-file emission and linking require
  explicit target adapters. The generic path never guesses those details.
- The full Limestone Unisel/Tunah/Bin2Bin/MachineIR orchestration is not compiled
  into standalone Flintstone. Applications can use those existing subsystems
  through their own selection/backend adapters when their dependencies are
  available. No D tests are implied by Flintstone's C/C++ tests.
- Tourchstone's other eight binding targets remain inventories. The current
  generated executable language binding is Lua.
- Decommissioned Yellowstone and its qBFD-dependent native loader/invoker are
  not silently re-enabled. A byte artifact with unresolved relocations is not
  treated as executable machine code.

These boundaries are deliberate diagnostics/API boundaries, not placeholder
success paths. Extend them by supplying semantics and tests at the owning
dialect, target or Stone-library adapter.
