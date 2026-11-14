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
process isolation remain separate, explicitly documented workstreams.

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
