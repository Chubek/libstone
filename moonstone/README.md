# Moonstone — LibStone program safety and native language integration

Moonstone is a **tested, early implementation slice** of four planned subsystems: Chisel, Syren, Supersonic, and ExoML. It is **not a complete production-grade safety compiler**. See `AGENTS.md` for the uncompromised implementation backlog and acceptance criteria, and [docs/STATUS.md](docs/STATUS.md) for the current implementation/validation checkpoint.

## Included working facilities

- **Core v1**: compiled C++20 API and versioned C source-management ABI; structured results/errors, strict UTF-8 immutable source snapshots and multiline byte spans, bounded diagnostics, synchronized symbol interning, proof/effect contracts, bounded S-expression parser, pattern unification and checked non-overlapping source edits.
- **Chisel**: forward CFG explicit-loan policy, shared/exclusive loans, moves, drops, conservative reinitialization/joins, final reachable-state diagnostics, explicit invalid-input/exhaustion status, finite region closure and demonstration nonmoving tracing heap.
- **Syren**: validated S-expression error-rule parsing, pattern-derived diagnostic messages, stderr and basic SARIF 2.1.0 serialization, bounded thread-safe runtime event recorder, explicit repair-validator callback and deterministic status/cost ranking.
- **Supersonic**: bounded enumerative integer-expression synthesis, portable checked signed arithmetic, strict grammar validation, explicit verification-oracle CEGIS with preserved proof assumptions, separate provisional legacy sampling, pure Lua `lssonic.synthgram` module.
- **ExoML**: a small, intentionally restricted executable DSL interpreter, explicit privilege gate for native host callbacks, typed StaticFFI declaration model, example ASDL and Aurocks grammar specifications.

## Build and run

From the `libstone` root:

```sh
cmake -S moonstone -B build-moonstone -DCMAKE_BUILD_TYPE=Debug
cmake --build build-moonstone --parallel
ctest --test-dir build-moonstone --output-on-failure
build-moonstone/moonstone-cli synthesize-demo
build-moonstone/moonstone-cli diagnose moonstone/syren/catalog/static/use-after-move.sexp '(use (moved x))'
```

Standalone builds need C/C++20 compilers, threads and CMake >= 3.20. No dependency is downloaded. `BUILD_SHARED_LIBS=ON` selects a shared core. CMake exports, installation, pkg-config metadata and isolated public-header checks are implemented. See [manual/03-core-v1.md](manual/03-core-v1.md) for C/C++ embedding, installation and sanitizer options.

`MOONSTONE_USE_MEMTKX`, `MOONSTONE_USE_SATIE`, and `MOONSTONE_USE_EQUINOX` default OFF. Enabling one now **fails explicitly** because functional adapters are not implemented; adding include paths is no longer presented as integration. `MOONSTONE_VENDOR_ROOT` remains reserved for future real adapters.

Link `Moonstone::Core`, `Moonstone::Chisel`, `Moonstone::Syren`, `Moonstone::Supersonic`, or `Moonstone::Exoml` (existing capitalization) and include the matching header, for example `<chisel/Chisel.hpp>`. Core is no longer header-only.

## Important limitations

- Chisel has a **small path-merged ownership state machine**, not a Rust-equivalent borrow checker. It requires explicit `EndBorrow` operations, does not model place projections or full NLL, and its join is deliberately conservative. It must not be used to establish memory safety of untrusted code.
- The example tracing heap does not scan stacks, move objects, implement finalizers, or integrate with MemTKX's runtime collection plans.
- Syren's syntactic patterns alone do not prove a diagnostic; call it only after a semantic violation was established by an analyzer. Repairs are verified only by a **caller-supplied** validator.
- Supersonic's native enumerator handles pure signed 64-bit arithmetic, not a full synthesis grammar or general SAT/SMT. `cegis_verified` requires an independent sound/exhaustive oracle and retains its assumptions; the legacy `cegis` optional-counterexample callback never sets `fully_verified`. Lua SynthGram remains separate and uses Lua numeric semantics, not checked signed 64-bit arithmetic.
- ExoML supports a restricted command subset, not the user's full pipeline/error syntax. Its `.g` and `.asdl` files are proposed formal frontend specifications, not a completed generated parser/AST pipeline. StaticFFI validates a declaration model but does not parse C headers, generate bindings, or execute ABI calls. Privileged callbacks execute host code and are **not** sandboxed.
- The expanded tests verify a narrow baseline, not comprehensive safety/security or real vendor integration. Root CMake now registers Moonstone and Wetstone. Limestone's missing VMWeave has been restored and tested; full-root validation still has an unresolved vendor compatibility blocker. See [docs/STATUS.md](docs/STATUS.md). No production-readiness claim is made.

## Vendor inventory inspected

The updated `master` tree of `github.com/Chubek/libstone` at commit `68bc6cf455c8f8e2e0c784e3a37307d684d1fa60` includes `third_party/memtkx`, `memtkx-c`, `satie`, `equinox-ng`, `exolangtk`, `cparser-iso11`, `ucpp`, `pkgconf`, `replxx`, `lua`, `kaguya`, `sfsexp`, `metatk`, `qamrpp`, and more. The ZIP does not vendor these or alter their licenses. Consult `docs/INTEGRATION.md` for the intended next steps.
