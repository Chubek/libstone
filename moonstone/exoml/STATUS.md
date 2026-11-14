# ExoML implementation status

This expansion completes a usable **static binding path**, not the full ExoML
workstream or Moonstone's production-safety acceptance plan.

| Implemented | Verification |
| --- | --- |
| Aurocks grammar → ASDL AST → source-preserving commands | Generator dependencies; `exoml_parser` syntax/location/budget/concurrency tests |
| Typed native auto-binding and checked conversions | `exoml_autobind`, including invalid input before invocation |
| Capabilities, thread affinity, exception diagnostics | `exoml_autobind`, original interpreter test |
| Atomic batch/module binding, filtering/renaming, aliases/constants | `exoml_autobind`, `exoml_generated` |
| Structured declaration validation and reflection | `exoml_static_ffi`, manifest JSON in `exoml_codegen` |
| Compiler-checked generated wrappers | `exoml_generated`: generated code linked to real C11 fixture |
| Compiler-measured ABI and layout expectations | `exoml_codegen`: probe JSON, deliberate signature/layout compile failures |
| Declarative workspace, Types helpers, transactional edits | `exoml_static_ffi` |
| DSL-to-wrapper path and CLI | `exoml_script`: CLI output compiled, linked and invoked |
| CLI/line REPL error handling | `exoml_cli`, including malformed input and resource limits |
| Installed CMake package | `exoml_package`: independent consumer configures, links and executes |
| Independent public headers | `moonstone_exoml_header_checks`, warnings as errors |
| Opt-in libclang C11 discovery and owned declaration/type graph | `exoml_extractor`: typedefs, qualifiers, arrays, recursive/anonymous records, callbacks, enums, partial errors and budgets |
| Deterministic discovery JSON, target/layout evidence and SHA-256 provenance | `exoml_discovery_cli`: independent JSON reader, C11 layout comparison, i386/x86-64 width changes, include/macro/source fingerprint changes |
| Pinned real MuLIB discovery (no generated bindings yet) | Optional inventory check: vec 7, JSON 20, parsec 25 functions; zero omitted functions, zero safe exports |
| Compiler-validated persistent normalization cache (Linux) | `exoml_cache`: cross-process hits, atomic concurrent publication, corruption/size/symlink rejection, source/provenance/flags/target/negative-include invalidation, real MuLIB hits |
| Bounded lossless internal cache codec and tool/source key identity | `exoml_cache_codec`: round-trip all model fields, malformed counts/enums/references, fingerprint and tool/limit changes |
| Safe compilation-database ingestion and target/ABI options | `exoml_discovery_context`: no driver/shell execution, strict input and flag rejection, working-directory/override semantics, native C11 ABI comparisons |
| Source-level macro provenance and final defined-state snapshots | `exoml_discovery_context`, discovery/cache tests: definitions/expansions/includes/skipped ranges, undef/redefine/push-pop, explicit unknown IDs |
| Initial P0.1 / gate A for pinned vec/JSON/parsec | Complete function and source-derived type inventories, macro/target/ABI/provenance cache invalidation; schema 2 and installed SDK checks |
| Versioned semantic contract model, sidecar parser and Gate B validation | `exoml_contract`: ownership/lifetime/error/effect/callback/destructor/concurrency contracts, deterministic round-trip serialization, contradictory-ownership/borrow-without-owner/length-mismatch/retained-without-unregister/unreviewed-export/version/drift rejection, projection to legacy OwnershipContract |
| CDecl JSON round-trip for contract validation | `parse_translation_unit_json`: declaration names, kinds, parameters sufficient for contract validation |

## Open work

- Beyond the initial gate: other compiler/platform/extension profiles, arbitrary
  flag vocabularies, full nested preprocessor replacement traces, parse-free TU
  caching, ucpp/CParser alternatives and package/version discovery. Unsupported
  inputs do not inherit completeness from the tested Linux/MuLIB profile.
- Direct C11 AOT adapter backend, On1x export backend and cross-analyzer
  integrations (Chisel foreign-call obligations, Wetstone external models,
  Flintstone FFI dialect). Discovery is always unsafe/unknown-effect and must
  not be treated as reviewed semantics.
- General pointer/handle ownership, aggregates by value, callbacks, variadic
  marshalling and calling-convention extensions. Explicit host adapters remain
  necessary; runtime ABI layouts are never guessed.
- DynamicFFI, extension loading/process isolation, CPython/Ruby emitters and
  XBind Lua integration. ExolangTk's inspected call engine reports `FFI_ENOSYS`;
  this implementation uses compiler-generated static calls.
- Pipelines, nested calls, arithmetic expressions, guards, imports, compile-time
  evaluation and the full proposed language. Unsupported syntax is rejected;
  `.g`/`.asdl` now describe the implemented syntax.
- Stable C embedding ABI/plugin table, replxx editing/history, native-call
  cancellation, production fuzzing/audit and cross-platform CI.

## Local verification

### C11 discovery increment

GCC 16.2.1 Debug and Release: **33/33** standalone tests each. Clang 22.1.8
Debug with ASan/UBSan: **33/33**. Extraction OFF: **29/29** existing checks
plus the new contract test. All use Prolific OFF, ExoML/Chisel/tests/tools ON.
Public ExoML headers compile independently with warnings as errors; installed
consumers exercise both runtime and optional extraction packages. Native C
layout probes match libclang evidence; 32-bit target evidence is compiler-only,
not an executed cross-target program. Details, exact commands, dependency
identities, MuLIB fingerprints and gaps are in
[the discovery report](manual/c11-discovery-report.md). Existing unrelated
Chisel/Supersonic/Syren warnings persist; no complete root/platform validation
is claimed.

The cache continuation adds two tests to the preceding 30-test discovery
baseline. It preserves raw/unknown trust and compiler validation on every hit;
it does not establish semantic-contract or AOT acceptance gates. See the
[cache continuation report](manual/discovery-cache.md#verification-report).
The final discovery context/provenance test closes the initial pinned-header
gate; exact evidence and limitations are in the
[gate-A report](manual/discovery-gate-a.md#verification-report).

### Semantic contract increment

The contract test (`exoml_contract`) exercises 49 checks: parsing, round-trip
serialization, all Gate B rejections (contradictory ownership, borrow without
lifetime source, pointer/length mismatch, retained callback without unregister,
unreviewed safe export, malformed version, unknown enums/effects, source
signature drift, duplicate functions/destructors), safe-export gating, and
projection to the legacy OwnershipContract. See
[the contract design](manual/semantic-contracts.md) for the full model and
validation rules.

### Earlier static-binding increment

- Linux GCC: all 36 standalone Moonstone CTests pass with tools/Python enabled.
- Linux Clang 22: all nine ExoML checks pass with AddressSanitizer and UBSan.
- Generated wrappers compile and invoke a real C library. Incorrect native
  signatures/layout expectations fail compilation, and probe JSON matches the
  independently compiled fixture's layout.
- Public ExoML headers compile independently on both compilers with warnings
  treated as errors. No complete LibStone root build or Windows validation is
  claimed by this increment.
- Clang static-analyzer checks pass on the interpreter, parser bridge,
  declaration/code generation implementation and standard-library workspace.
