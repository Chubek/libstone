# `scripts/acsem` parity audit

Audit date: 2026-11-21. The comparison uses executable implementation, not the
broader Action Semantics expansion roadmap. The reference is
`scripts/acsem/acsem.pl`, particularly `parse_expr`, `parse_primitive`,
`parse_action`, `type_expr`, `type_action`, `run_expr`, and `run_action`, together
with `Acsem/Emit.pm` and the backend tests.

## Semantic inventory

All implemented script action and yielder operations already exist in the
Novostone Aurocks grammar, ASDL AST, canonical lowering, and four runtimes:

| Script construct | Existing Novostone representation |
| --- | --- |
| Integer, field, identifier-token literals | `Integer`, `Field`, `Token` |
| `given`, `bound`, `contents` | `Given`, `Bound`, `Contents` |
| Checked i32 `+`, `-`, `*`, `/`, `==`, `<` | `Add`, `Sub`, `Mul`, `Div`, `Equal`, `Less` |
| `complete`, `fail`, `give`, `check`, `escape` | Corresponding existing action opcodes |
| `bind`, `store`, `allocate cell`, `evaluate` | `Bind`, `Store`, `Allocate`, `Evaluate` |
| `then`, `and then`, `hence` | `Then`, `AndThen`, `Hence` |
| `otherwise`, `trapping`, `furthermore` | `Otherwise`, `Trapping`, `Furthermore` |
| Multiple syntax sorts, forward references, finite inhabitation | Existing syntax validation |
| Constructor/field/rule checks, known-tag checking | Existing elaboration and runtime checks |

There is no missing script semantic opcode to add or renumber. The script
rejects closures, unfolding, runtime data declarations, pattern matching,
generators, and continuations, which Novostone already implements. Neither
implementation at audit time implements rewrite attachments or Lua; those are
new facilities, not script parity.

## Actual interface gaps at audit time

| Interface | Script | Novostone |
| --- | --- | --- |
| Executable targets | C, C++, Python, Rust, Pascal, Java, Perl | C, Python, Rust, Pascal |
| Model export | JSON version 1 | No JSON export |
| Documentation target | None | LaTeX |
| Generator selection | `--emit` | `--target` |
| Run specification directly | `--run`, also default mode | No direct runner |
| Generated interpreter input | stdin, `--term`, `--term-file`, optional `--run` | stdin |
| Java generated class naming | `--class-name` | No Java target |
| Option placement | Permuted options | Fixed CLI ordering |
| Check output | `OK: NAME` | `NAME: valid` |
| Diagnostic prefix | `ACSEM_ERROR:` / `ACSEM_LIMIT:` | Backend-specific diagnostics |

Matching existing successful and failed execution observations is distinct from
changing public interfaces. Extra targets need actual standalone implementations;
they must not be advertised as supported by aliases to unrelated interpreters.
JSON export also needs a documented schema rather than an unsupported promise of
loading the Perl reference model into Novostone.

Both implementations use exit codes 0 for completion, 1 for semantic
failure/escape, and 2 for invalid input or resource exhaustion. Error text and
some boundary policies differ. In particular, the script lexer separates minus
from its digits whereas the Novostone specification grammar uses one signed
integer token; script literal tokens are interned on evaluation while Novostone
preloads literal tokens. Dialect 2 intentionally has different step accounting.
These differences should not be silently "fixed" by altering old opcode or
resource semantics.

## Executable differential gate

`tests/parity.py` compiles the actual script example specifications and an
additional combinator corpus with Novostone, then compares each program against
the Perl reference's exit status and stdout. It exercises all script semantic
forms, dynamic tag failures, persistent storage through recovery, arithmetic
boundaries, transients, lexical scope, and malformed terms. The same corpus runs
for C, Python, Rust, and Pascal; `--dialect 2` repeats the semantic comparison
through the extended machine. It deliberately does not require identical
diagnostics or resource counts.

```sh
python3 novostone/ActionSemantics/tests/parity.py \
  --cli BUILD/ActionSemantics/acsem \
  --reference scripts/acsem/acsem.pl \
  --work BUILD/ActionSemantics/test-parity-c \
  --target c --compiler cc
```

Use `--target python --compiler python3`, `--target rust --compiler rustc`,
or `--target pascal --compiler fpc` for the other backends. `--flags` carries
generated-C sanitizer flags. Compilation uses the same strict warning and
runtime-check policies as the existing backend corpus.

## CLI compatibility additions

The Novostone CLI now accepts `--emit` as an alias for `--target`, `--output`
as an alias for `-o`, long value options in `--name=value` form, options before
or after the specification path, `-h`, and `--` to end option parsing.
Generation still completes validation before opening an output file.
Conflicting modes, duplicate output destinations, extra specification paths,
unknown options, and output requests with `--check` are diagnosed.

`tests/parity_cli.py --cli BUILD/ActionSemantics/acsem --work WORKDIR` checks
those interfaces across all five existing generation targets, including
permuted dialect/budget arguments and preservation of existing output files
on errors. Extra executable targets, model export, direct execution, and
generated-interpreter argument handling remain separate gaps.

### Audit verification

The existing Debug library/executable under
`/mnt/nvme/chubak-edsac-acsem-build/debug` passed **138 differential observations
per backend and dialect**: C, Python, Rust, and Pascal, with dialects 1 and 2
(1,104 observations total). This establishes parity for the tested legacy
subset; it is not verification of concurrent rewrite changes.

The updated `main.cpp` was independently compiled with C++20 and
`-Wall -Wextra -Wpedantic -Werror` against that existing library; its isolated
CLI compatibility suite passed. Parent integration is responsible for rebuilding
the shared frontend/runtime changes and registering these new suites in CTest.
Generated artifacts reside in `/mnt/nvme/acsem-parity-audit`; the preferred
temporary volume was full.
