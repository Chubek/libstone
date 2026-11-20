# Cephyr SDK (C ABI v1)

`<cephyr/CephyrSDK.h>` lets an embedding application extend a Cephyr compilation
with explicit, **trusted in-process callbacks**. Link against `Cephyr::Cephyr` or
`libcephyr`. Create a `cephyr_sdk` with `struct_size = sizeof(cephyr_sdk)` and
`abi_version = CEPHYR_SDK_ABI_VERSION`, then set `cephyr_request.sdk` for that call.
A null SDK leaves the normal pipeline unchanged. Cephyr does not search for,
load, or sandbox user plugins. Only register code that you would trust to run
with your application's privileges.

| Stage | Input | Result |
| --- | --- | --- |
| `CEPHYR_SDK_DESUGAR` | Raw C translation unit, before CPP | Optional replacement C text. This stage also runs for raw C with `-E`. It does not see the contents of includes and is skipped for `.i` and assembler inputs. |
| `CEPHYR_SDK_ANALYZE` | Immutable MIR v1 text just after Cephyr has parsed, checked, and lowered the C translation unit | Additional diagnostics or rejection. Runs for C and `.i`, including `-fsyntax-only`. |
| `CEPHYR_SDK_REWRITE` | MIR v1 text after analysis | Optional replacement MIR. Runs for `--emit-mir` and native output, not for `-fsyntax-only`. |
| `CEPHYR_SDK_ASSEMBLY` | Final MIR v1 text for an exact target triple | Required assembly output, replacing the built-in backend for `-S`, `-c`, and linking from C/`.i`. |

Each callback receives the stage, input name, resolved triple, architecture and
ABI. Hooks run once per applicable input in pipeline order, in registration
order within a stage. A successful transform's output is the next hook's input.
Cephyr emits canonical MIR from lowering but does not reserialize callback
output between hooks; rewriters should preserve the canonical form. Cephyr
verifies the final rewritten MIR, not each intermediate rewrite result.
The analysis stage only sees the pre-rewrite MIR, so an analysis callback should
not assume that later rewrites preserve properties it checks unless the
application guarantees that. Names must be unique within an SDK. An empty target
selector applies to all targets except for assembly, which requires a concrete
triple; two backend hooks cannot claim the same triple. A nonmatching backend is
skipped and Cephyr uses its built-in backend.

## Text formats and locations

The source hook is a **text desugaring** interface, not parser grammar
registration. As it runs before CPP, it sees unexpanded macros, comments and
contents of conditional regions, including inactive ones. Downstream compilation
runs the normal preprocessor, parser and lowering on the transformed source.
`-E` stops after preprocessing; it does not parse or type-check the result.
There is no SDK source-map transport: later diagnostics refer to the
transformed input unless ordinary preprocessor line directives carry other
locations. Preserve line breaks or emit appropriate `#line` directives if
locations matter; those directives do not preserve original columns. When
reporting their own diagnostics, hooks can supply a file, line and column from
their own mapping.

The public interface has no Cephyr AST structures, private C++ types, or
mutable compiler IR handles. Use `mir_schema_version` before interpreting MIR;
its text and invariants are described in
[Archon’s MachineIR documentation](../Archon/MachineIR.md).
MIR is lowered, not a textual representation of all C syntax or types.

The built-in native backend parses and verifies the MIR it consumes. Cephyr
also parses and structurally verifies rewritten `--emit-mir` output and MIR
passed to a custom assembly callback. This does not prove equivalence, preserve
the C ABI automatically, or test custom assembly. Rewrites must preserve
externally visible function names and signatures, globals, and the contract
with Cephyr's linkage/data information, which sits outside MIR. Do not assume
that a rewrite can introduce new encodings, architectures, or MIR opcodes.

An assembly callback must emit target-compatible assembly, including definitions
for all MIR functions and all MIR globals with nonzero word counts. Honor
`CEPHYR_SDK_POSITION_INDEPENDENT` when set. For C-local definitions owned by MIR,
emit exactly one `\t.globl SYMBOL\n` for the MIR name (as the built-in backend
does); Cephyr replaces it with `.local`. Zero-word MIR globals are declarations;
do not define them. Some of them refer to C data that cannot be represented as
MIR words (for example, a narrow global); Cephyr appends those definitions from
its separately lowered data assembly.

`-S` returns assembly without assembling it; `-c` and links run the normal
target tools. An explicitly selected Lua triple can also name an SDK-only
architecture. Its `abi` must be `LP64` and the request must register an assembly
hook for that exact triple. The frontend uses a little-endian LP64 C data model
with unsigned plain `char` and no architecture-specific predefined macros for
such targets. This does not provide the ISA or calling convention: the hook
owns the ABI and must emit compatible assembly. SDK-only targets are not
auto-detected and cannot link Cephyr's bundled startup/libc. This hook does not
replace the assembler or linker.

## Memory, errors, and limits

Keep the SDK descriptor, descriptor array and callback state alive for the
call to `cephyr_compile`. Inputs are read-only borrows for the lifetime of each
callback. Pass a length-delimited span to `output->set_text` or `output->report`;
the host copies it before returning, so the callback can use stack storage.
Neither the input spans nor the output sinks/context may be used after the
callback returns. Do not reenter `cephyr_compile` on the callback thread or
throw through the C API. If using the same registration from concurrent calls,
synchronize mutable callback state.

Return `CEPHYR_SDK_OK` to commit or `CEPHYR_SDK_REJECT` to stop. Error diagnostics
and `-Werror`-promoted warnings also stop compilation. A failed sink call
rejects the callback even if it returns `CEPHYR_SDK_OK`. A failing transformation
does not publish its current translation unit's output; its diagnostics are
retained. This is not request-wide rollback of earlier translation units or of
callback side effects.

An invalid registration fails `cephyr_compile` with `CEPHYR_INVALID_ARGUMENT`;
callback failures are `CEPHYR_COMPILE_ERROR`. Inspect the compilation result
with `cephyr_result_diagnostics` for details, then destroy the result.

The v1 limits in the header include 64 hooks and 256 callback diagnostics per
compile call, and at most 16 MiB of replacement C, 4 MiB of replacement MIR, or
16 MiB of assembly per hook. Submitted text must not contain NUL bytes. These
limits protect the host's copies; they cannot enforce execution time or
isolate memory accesses in trusted callbacks.

See [`examples/sdk_desugar.c`](examples/sdk_desugar.c) for a small C embedding
example and [`tests/sdk.c`](tests/sdk.c) for a complete pipeline with diagnostic,
optimization, and backend callbacks.
