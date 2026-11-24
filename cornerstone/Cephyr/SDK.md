# Cephyr SDK (C ABI v1)

`<cephyr/CephyrSDK.h>` lets an embedding application extend a Cephyr compilation
with explicit, **trusted in-process callbacks**. Link against `Cephyr::Cephyr` or
`libcephyr`. Create a `cephyr_sdk` with `struct_size = sizeof(cephyr_sdk)` and
`abi_version = CEPHYR_SDK_ABI_VERSION`, then set `cephyr_request.sdk` or pass
the SDK to `cephyr_context_compile` for that call.
A null SDK registers no native extensions. The native SDK does not discover
or sandbox callback code. Only register code that you would trust to run with
your application's privileges. To load a trusted native plugin from an explicit
file path, see [configuring extensions and plugins](extension/CONFIGURATION.md).

To inspect a syntax tree without lowering, pass your SDK on the request to
`cephyr_parse_c11`, or use `--emit-ast=json|yaml|sexp` in the driver arguments.
Raw C runs desugar and directive hooks; AST emission does not run MIR hooks.
Compiling a validated syntax document skips desugar and directives and runs the
normal post-lowering hooks. See [C11 syntax documents](docs/C11Syntax.md).

| Stage | Input | Result |
| --- | --- | --- |
| `CEPHYR_SDK_DESUGAR` | Raw C translation unit, before CPP | Optional replacement C text. This stage also runs for raw C with `-E`. It does not see the contents of includes and is skipped for `.i` and assembler inputs. |
| `CEPHYR_SDK_DIRECTIVE` | Arguments of an active `#pragma cephyr NAME ...`, `#pragma GCC NAME ...`, or equivalent `_Pragma` in raw C | Optional insertion of already-preprocessed C. Runs once per occurrence, including with `-E`; register in `directives`, not `hooks`. |
| `CEPHYR_SDK_ANALYZE` | Immutable MIR v1 text just after Cephyr has parsed, checked, and lowered the C translation unit | Additional diagnostics or rejection. Runs for C and `.i`, including `-fsyntax-only`. |
| `CEPHYR_SDK_REWRITE` | MIR v1 text after analysis and any opt-in QML passes | Optional replacement MIR. Runs for `--emit-mir` and native output, not for `-fsyntax-only`. |
| `CEPHYR_SDK_ASSEMBLY` | Final MIR v1 text for an exact target triple | Required assembly output, replacing the built-in backend for `-S`, `-c`, and linking from C/`.i`. |

Each callback receives the stage, input name, resolved triple, architecture and
ABI. Hooks run once per applicable input in pipeline order, in registration
order within a stage. A successful transform's output is the next hook's input.
Cephyr emits canonical MIR from lowering but does not reserialize callback
output between hooks; rewriters should preserve the canonical form. Cephyr
verifies the final rewritten MIR, not each intermediate rewrite result.
The default [Blackstone pass](README.md#blackstone-mir-optimization) runs
after SDK rewrites; assembly hooks receive its output.
The analysis stage only sees the pre-rewrite MIR, so an analysis callback should
not assume that later rewrites preserve properties it checks unless the
application guarantees that. Hook names must be unique within an SDK. An empty target
selector applies to all targets except for assembly, which requires a concrete
triple; two backend hooks cannot claim the same triple. A nonmatching backend is
skipped and Cephyr uses its built-in backend.

## Custom directives

Add `cephyr_sdk_directive` descriptors to `cephyr_sdk.directives` and set
`directive_count`. Each descriptor contains a `name`, `target_triple`,
`callback`, and `user` pointer. Names must be case-sensitive ASCII C
identifiers. The `(name, target_triple)` pair must be unique. An empty target
selector is a fallback for that name; a matching exact triple takes precedence
regardless of registration order. The original ABI v1 prefix is still accepted
and registers no directives; both appended fields must be present to use them.

```c
#define TEXT(s) { (s), sizeof(s) - 1 }

static uint32_t emit_answer(const cephyr_sdk_input *in,
                            const cephyr_sdk_output *out, void *user)
{
    (void)user;
    if (in->text.size != 0)
        return CEPHYR_SDK_REJECT; /* This directive takes no arguments. */
    const cephyr_sdk_text declaration = TEXT("int answer(void) { return 42; }");
    return out->set_text(out->context, declaration) ? CEPHYR_SDK_OK : CEPHYR_SDK_REJECT;
}

static const cephyr_sdk_directive directives[] = {
    { TEXT("emit_answer"), {0}, emit_answer, NULL },
};
static const cephyr_sdk sdk = {
    .struct_size = sizeof(cephyr_sdk),
    .abi_version = CEPHYR_SDK_ABI_VERSION,
    .directives = directives,
    .directive_count = 1,
};
```

Use `#pragma cephyr emit_answer` or `#pragma GCC emit_answer` in the C
translation unit. Both spellings also work with `_Pragma`, for example
`_Pragma("GCC emit_answer")`. The two namespace tokens are case-sensitive and
select the same registration. Under `cephyr`, a missing name or a name with
no matching registration is an error. Under `GCC`, an unclaimed pragma keeps
its ordinary behavior; this alias does not implement GCC's built-in pragmas.

The C preprocessor recognizes these in active branches and included files,
not in comments, string contents, or inactive branches. A macro can produce
`_Pragma`; `_Pragma` inside a preprocessing directive such as `#if` is not
supported for Cephyr extensions. Cephyr does not dispatch directives from
`.i` or assembler inputs.

After ucpp finishes successfully, Cephyr calls each matching directive in
occurrence order on the compiling thread, without holding the ucpp lock. The
callback receives `CEPHYR_SDK_DIRECTIVE`, `directive_name`, the presumed
`input_name` and `line` (honoring `#line` and include context), and `column`
(currently zero, meaning unknown). For `_Pragma` the location is the expansion
site. These appended fields are empty or zero for ordinary hooks. `text`
contains only the argument tokens: Cephyr trims leading and trailing trivia
and normalizes intervening whitespace or comments to one space, preserving
token spellings. `#pragma` arguments are not macro-expanded. `_Pragma` operands
are destringized normally before the tokens are collected. With `report`,
passing an empty file and zero coordinates uses the pragma's presumed location.

`set_text` inserts a fragment at the pragma's output position. Omit the call
or submit an empty span to emit nothing. This is **already-preprocessed C**:
macro names are not expanded and the fragment cannot define macros, open
headers, change conditional compilation, or dispatch another pragma. Submit
complete tokens in valid UTF-8, with LF line endings, no comments, preprocessing
`#`/`##` tokens, `_Pragma`, or stray backslashes. C syntax and semantics are
checked by the normal frontend except with `-E`, which stops after preprocessing.
Cephyr separates the fragment from surrounding tokens with line breaks and
generates line markers (omitted with `-P`). The first emitted line maps to the
pragma's logical line, subsequent emitted lines count from there, and following
original tokens retain their original logical file and line. Inline
`_Pragma` insertions can change columns and split a physical output line.

## Declarative QML profiles

An embedding client can opt into QML without registering callbacks. Supply
`--profile`, `--qml-pass`, `--qml-file`, `--qml-fuel`, and `--qml-param` in
`cephyr_request.argv` exactly as on the CLI, for example
`{"--emit-mir", "--qml-pass=canonicalize", "-o", "-", "-"}` with the source
in `stdin_data`. C++ and Python clients pass the same arguments to `compile`.
`--config FILE` selects a TOML file containing profiles; the usual
`CEPHYR_CONFIG` and working-directory defaults also apply. See
[QML optimization profiles](README.md#qml-optimization-profiles) for settings,
precedence, dependencies, and trust requirements. The request and SDK ABI are
unchanged.

Analysis callbacks see MIR before QML. Changed QML output is structurally
verified before any SDK rewrite callback runs. This is not an equivalence proof.

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
for all MIR functions and all MIR globals with nonzero word counts. Use
the exact MIR names, including ASCII placeholders for Unicode identifiers.
Cephyr restores the external UTF-8 symbol spelling during ELF assembly emission. Honor
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

Keep the SDK descriptor, hook and directive arrays, and registration strings alive and
unchanged for the call to `cephyr_compile` or `cephyr_context_compile`.
A [persistent context](EMBEDDING.md#reusing-a-context-and-loading-trusted-code-c-api)
does not store its per-call SDK. Keep callback state alive for the same
duration; if using it from concurrent calls, synchronize mutable state.
Inputs are read-only borrows for the lifetime of each callback. Pass a
length-delimited span to `output->set_text` or `output->report`; the host copies
it before returning, so the callback can use stack storage. Call each sink with
the exact supplied `output->context`, on the callback's thread before it
returns. Do not save either interface for later or pass it to another thread.
Do not reenter either compile function on the callback thread or throw through
the C API.

Return `CEPHYR_SDK_OK` to commit or `CEPHYR_SDK_REJECT` to stop. Error diagnostics
and `-Werror`-promoted warnings also stop compilation. A failed sink call
on the active callback thread rejects the callback even if it returns
`CEPHYR_SDK_OK`. A failing transformation
does not publish its current translation unit's output; its diagnostics are
retained. This is not request-wide rollback of earlier translation units or of
callback side effects.

An invalid registration fails the compile call with `CEPHYR_INVALID_ARGUMENT`;
callback failures are `CEPHYR_COMPILE_ERROR`. Inspect the compilation result
with `cephyr_result_diagnostics` for details, then destroy the result.

The v1 limits in the header include 64 hooks, 64 directive registrations, up to
4,096 directive invocations and 256 callback diagnostics per compile call, and
at most 16 MiB of replacement C, 4 MiB of replacement MIR, or 16 MiB of assembly
per hook. Each directive has up to 64 KiB of arguments and up to 64 KiB of output.
Cephyr limits captured directive input (including names and filenames) to
16 MiB per translation unit and directive output to 16 MiB per compile call.
Submitted text must not contain NUL bytes. These
limits protect the host's copies; they cannot enforce execution time or
isolate memory accesses in trusted callbacks.

See [`examples/sdk_desugar.c`](examples/sdk_desugar.c) for a small C embedding
example and [`tests/sdk.c`](tests/sdk.c) for a complete pipeline with diagnostic,
optimization, and backend callbacks.

## Python

The Python [embedding API](EMBEDDING.md#python) accepts `sdk=` on both
`Compiler.compile()` and `cephyr.compile()`. Pass an iterable of `Hook`
descriptors. They use the same v1 hook stages, ordering, exact target matching,
and [limits](#memory-errors-and-limits) as C. The Python hook wrapper does not
expose the native `cephyr_sdk_directive` registration array.

```python
from cephyr import Diagnostic, Hook, HookResult, Severity, Stage, compile

def desugar(data):
    if data.text == b"export_answer 42;\n":
        return "int answer(void) { return 42; }\n"
    return None

def analyze(data):
    return HookResult(diagnostics=(
        Diagnostic(Severity.NOTE, "sdk.checked", "Inspected lowered MIR"),
    ))

result = compile(
    ["--target=x86_64-linux-gnu", "--emit-mir", "-o", "-", "-"],
    "export_answer 42;\n",
    sdk=[
        Hook(Stage.DESUGAR, "export-answer", desugar),
        Hook(Stage.ANALYZE, "inspect", analyze, target_triple="x86_64-linux-gnu"),
    ],
)
```

- A hook receives an immutable `HookInput` with copies of `text`, `input_name`,
  `target_triple`, `target_architecture`, and `target_abi` as `bytes`. It also
  includes `stage`, `mir_schema_version`, and `flags`. An assembly hook can
  use `data.position_independent` to check whether it must emit PIC.
  The input can be retained after compilation. No native sinks are exposed.
- Return `None` to leave text unchanged, `str` or `bytes` to replace it, or
  `HookResult(replacement=..., diagnostics=(...), reject=...)` to combine a
  replacement with diagnostics or explicitly reject the input. `str` values are
  encoded as UTF-8; use `bytes` for verbatim SDK text. An empty replacement is
  distinct from `None` and is allowed only for `DESUGAR`. `ANALYZE` cannot
  replace text. `ASSEMBLY` requires an exact target and must emit assembly
  according to the [native backend contract](#text-formats-and-locations).
  On explicit rejection the diagnostics are reported but the replacement is ignored.
- Explicit rejection, error diagnostics, and native sink validation failures
  return a `Result` with `Status.COMPILE_ERROR`. If a hook raises, or returns
  the wrong Python value type, the wrapper rejects the native request and
  re-raises the Python exception after releasing the native result.
- Callbacks run synchronously on the compiling thread. Do not reenter Cephyr
  from that callback thread. Requests can share hook descriptors across
  threads, but must synchronize mutable callback state. Python hooks are
  trusted in-process code, not plugins loaded or sandboxed by Cephyr.
