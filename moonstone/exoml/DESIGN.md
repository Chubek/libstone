# ExoML static binding expansion

## Contracts

Static calls use the C++ compiler's actual function type and ABI. `AutoBind`
deduces free functions, noexcept functions and non-overloaded callables, checks
arity and value conversion before invoking them, and copies returned strings.
Native calls are privileged by default. Registration and module installation
are atomic; failed calls do not publish a result. Calls are synchronous on the
interpreter's caller thread; optional registration-thread affinity is enforced.
Interpreter instances and library builders require external synchronization.

The existing declaration model remains source-compatible. It gains exported
names, constants, target layout expectations and structured validation. Code
generation includes original headers, checks exact signatures, and emits typed
registration code. Layout probes use `sizeof`, `alignof` and `offsetof` in the
target compiler; validation never guesses layouts. Unsupported declarators,
variadic wrappers and unrepresentable ownership policies fail explicitly.

The generated Aurocks parser constructs arena-owned ASDL nodes. The C++ bridge
preserves byte source spans and lowers to the existing command API. Input,
command and argument budgets bound accepted modules. The runtime revalidates
caller-built commands before calling host code. Values add unsigned integers,
real numbers and null; existing alternatives retain their order. `$@` refers
to the last successful result. Syntax and service failures carry source spans.

StaticFFI's declarative services build and inspect in-memory declarations and
return generated source as text. File reads/writes and compilation are host/CLI
operations. No code is dynamically loaded during declaration evaluation.

## Reuse and boundaries

Keep Moonstone's `OwnershipContract`, diagnostics, declaration types and service
registry. Use repository Aurocks/ASDL generators rather than a second DSL parser.
XBind's manifest is suitable for an explicit optional export adapter; unknown
ownership/effects must not disappear in that export. Its Lua emitter is a
separate backend. The inspected ExolangTk `ffi_call` returns `FFI_ENOSYS` and
cannot implement static calls. The inspected CParser fixes integer widths in
its AST and documents incomplete preprocessed-input support; it is not an ABI
authority. Full C11 extraction, dynamic loading, Python/Ruby emitters and
process isolation remain separate, explicitly documented workstreams. An opt-in
libclang extractor now supplies the first C11 discovery slice described below.

## C11 declaration discovery (initial P0.1 / gate A)

`cdecl::TranslationUnit` is an owned typed graph, separate from the legacy
`BindingModule`'s lossy spelling fields. Type IDs retain canonical identity and
source spelling; typedef/elaborated edges, qualifiers, recursive records,
fields/bitfields, arrays, enums and callback signatures are explicit. Source
locations, diagnostics, target/compiler identity and SHA-256 input provenance
travel with the unit. JSON is a deterministic interchange projection, not the
internal analysis interface. This discovery schema version 1 is **not** old
binding-reflection version 1 and confers no semantic-contract trust.

The libclang driver is isolated in optional `Moonstone::ExoMLExtract`; no Clang
or OpenSSL dependency reaches `Moonstone::ExoML`. It parses the requested header
directly, with a restricted option vocabulary, without executing a shell or any
discovered code. Only declarations originating in that header are exported;
referenced dependency types and all parsed include fingerprints remain in the
graph/evidence. Compiler errors and budget failures preserve partial evidence
and force `complete=false`. An unsupported construct remains recorded rather
than becoming a guessed signature. The initial gate for pinned vec/JSON/parsec is
implemented and tested; generalized compiler/extension/platform coverage,
semantic verification and generator consumption remain separate work.

Discovery schema 2 adds `CompilationContext` and `Preprocessing` to the same
owned IR. Strict bounded JSON ingestion never runs compilation commands and
rejects unclassified flags instead of losing ABI input. Source directory/driver,
original/ignored arguments and database fingerprints remain reviewable. Actual
compiler/target/options scope type IDs; callers also retain the unit fingerprint
when comparing source revisions. Source tokens preserve lexical declaration and
parameter spelling alongside normalized compiler types.

Compiler-reported macro definitions, source expansion links, includes and skipped
ranges accompany a final defined-state snapshot obtained with read-only compiler
probes. No arbitrary replacement is evaluated by the probe, and no missing
definition ID is guessed. Nested replacement traces remain explicitly outside
libclang's exposed API; unavailable/unsupported evidence stays unknown or fails
with diagnostics. Both compiler passes and reread input fingerprints guard
against mixed source/compile-database/provenance inputs. The internal cache codec
is versioned with these additive fields and round-trips all of them.

The optional Linux normalization cache reparses before every lookup and binds
its content key to a freshly serialized compiler AST, parsed file/provenance
hashes, target/options, limits and build-source identity. This handles negative
include probes and new shadow headers without a guessed dependency manifest.
An internal bounded binary codec traverses the same typed model in both
directions; it is not a second signature database or native-object dump.
Private no-follow file access and atomic publication protect local cache I/O.
Cache activity lives in `CacheReport`, outside deterministic reflection. Native
AST bytes are temporary hash material only and are never loaded from cache.
This is not parse-free TU caching (not required for the initial gate); see
[discovery-cache.md](manual/discovery-cache.md) and the
[gate-A closeout](manual/discovery-gate-a.md).

See [the feature design](manual/c11-discovery.md) for security boundaries and
failure semantics, and [the report](manual/c11-discovery-report.md) for executed
tests and pinned MuLIB coverage.

## Failure semantics and verification

Malformed model data produces diagnostics; emission refuses any error. Type,
range, capability, affinity and arity failures occur before invocation. Native
exceptions become evaluation diagnostics (native side effects cannot roll back).
Pointers that might escape need explicit host adapters; no integer-to-pointer
coercion or inferred ownership is provided.

Regression coverage includes numeric boundaries, wrong types/arity, strings,
nullable values, exceptions, duplicate/atomic installation, capabilities,
thread affinity, malformed AST/syntax, budgets, declaration injection attempts,
ownership conflicts, generated signature mismatch, compiled ABI probes and
generated wrappers linked to a real C fixture. Build public headers separately,
run all Moonstone CTests, and run focused Clang ASan/UBSan checks.
