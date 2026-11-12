# Implementation checkpoint

This is a **partial implementation**, not completion of `AGENTS.md` phases 0–4.
The original six tests passed before changes. The core/build-first increment is
designed in `core-v1-design.md`; embedding contracts are in `../manual/03-core-v1.md`.

## Delivered in this increment

- Compiled/versioned core with a narrow stable C source-management ABI, C++
  structured errors/results, strict UTF-8 snapshots, multiline byte spans,
  synchronized bounded symbol/source storage and bounded diagnostic retention.
- Portable checked signed arithmetic; strict synthesis grammar/arity validation,
  accurate candidate budgets, syntax-sensitive predicate enumeration, explicit
  verified-oracle CEGIS and provisional legacy sampling. Oracle assumptions are
  retained; only a genuinely failing new counterexample is accepted.
- Chisel rejects unknown operations/malformed CFGs, owner reads under exclusive
  loans and possibly-live reinitialization. Diagnostics are emitted from final
  reachable inputs, without collapsing distinct instructions on default spans.
  Analysis exposes invalid-input/exhaustion and failure causes. This remains a
  whole-place explicit-loan policy, not a memory-safety certificate.
- S-expression control-whitespace round trips and ambiguous source-edit rejection;
  Syren status/cost/ID ranking now satisfies a strict ordering, with independent
  validator status regression coverage. ExoML rejects unknown caller-built AST
  operators before host calls.
- Static/shared CMake packages, install component, pkg-config metadata, isolated
  public-header compilation, external installed C/C++ consumers, sanitizer
  options and GCC/Clang/MSVC CI definitions. Unsupported vendor switches fail
  configure with a precise error; no nominal header-only integrations are enabled.
- Root CMake registration for Moonstone/Wetstone; Wetstone export paths no longer
  embed build-only dependency directories. No vendored code is modified/copied.

## Local verification

Environment: Linux, GCC 16.2.1, Clang 22.1.8. Tests cover only the capabilities
above; they do not establish production safety. See the recorded build directories
under `/tmp/opencode` in the implementation session for detailed logs.

- GCC Debug and clean Release/static: 12/12 standalone checks and isolated headers.
- Clang Debug/shared + ASan/UBSan: 12/12 standalone checks and isolated headers.
- GCC TSan/core-only configuration: 4/4 checks, concurrent sources/symbols, C ABI and package
  consumers. Other components were disabled in this targeted concurrency run.
- Installed external C and C++ programs compile, link and execute after package
  relocation in both static and shared test configurations; the installed CLI
  also runs. Missing required package components and unsupported vendor options
  are executable configure-regression tests.
- A standalone consumer also compiled, linked and ran using the relocated Unix
  pkg-config metadata and a C++ linker.
- Moonstone's executables build through root CMake with Limestone explicitly OFF;
  all 12 Moonstone checks pass there. This is target-level integration,
  **not a successful full root build**.
- Windows/MSVC CI is supplied but has not been executed in this Linux environment.
- Clang static analyzer and selected bug-prone checks pass on the compiled core
  and changed subsystem regression translation units. The deliberate invalid-enum
  negative fixtures are excluded from the analyzer's enum-cast warning, not from
  runtime regression/sanitizer execution.

## Root integration follow-up

1. **Resolved:** at the user's request, all 38 files in `limestone/vmweave/`
   were restored byte-for-byte from commit
   `dab8c50aa8b2745ee0fe9d4a2bc57944bd2b33b1`, the parent of removal commit
   `1e607cb25`. The archived directory was left untouched. Default root CMake
   configuration now succeeds with Limestone enabled; VMWeave's focused build
   and all six tests pass, including generated C and native execution.
2. The earlier complete root build with `-DLIBSTONE_BUILD_LIMESTONE=OFF` failed
   in Wetstone's vendored Satie/Equinox inclusion: both
   `third_party/satie/include/DSLtk.hpp` and
   `third_party/equinox-ng/DSLUtils.hpp` define `dsl::FixedString`, `dsl::DSL` and
   other overlapping facilities. No header guards/namespaces were bypassed and
   no vendor adapter was silently disabled to make this pass. Other root targets
   may still have failures beyond this first observed blocker. A complete root
   build has not been revalidated after the VMWeave restoration.

## Still required

- Core semantic memory/type/control-flow IR and verified source/identity-preserving
  Flintstone/Wetstone adapters. Existing Wetstone adapters are **not** claimed as
  sound Moonstone adapters.
- Chisel projections, reborrows, provenance, two-phase loans, NLL, exceptional
  edges, liveness, interprocedural summaries and the full soundness acceptance
  corpus. Real MemTKX plans, mutator/root/barrier protocol and stress collectors.
- Typed unified native/Lua SynthGram, Kaguya, actual Satie proofs and Equinox-guided
  synthesis with pinned linked integrations and independent proof checking.
- Strict expanded Syren rule schema/catalog, semantic witnesses, rich streaming
  SARIF, Unicode/URI conversions, symbolized/redacted runtime diagnostics and
  independently compiler-validated generated repairs. Basic SARIF currently
  lacks comprehensive multiline/related/fix data; SourceManager byte columns
  must not be mistaken for SARIF UTF-16 columns on non-ASCII text.
- Generated ExoML grammar/ASDL frontend, full stdlib, C11 preprocessing/parsing,
  target ABI probes, real FFI, REPL, native bindings and process-isolated plugins.
- Full cross-platform/root acceptance, vendor revision locking, fuzz corpus,
  benchmarks, generated API documentation and independent security audit.

The next feature should start with semantic IR/adapter design and resolving the
root vendor compatibility issue, not with advertising new safety guarantees.
