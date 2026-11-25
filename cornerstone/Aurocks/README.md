# Aurocks

`aurocks` is a native, scannerless parser generator. It reads the
Aurocks `.g` grammar format and emits C99. The default portable output is
one C translation unit and does not require Perl, a generator executable,
or a parser library at runtime.

## Build and run

From the repository root:

```sh
cmake -S cornerstone/Aurocks -B build/aurocks
cmake --build build/aurocks
ctest --test-dir build/aurocks --output-on-failure

build/aurocks/aurocks grammar.g > parser.c
cc -std=c99 -c parser.c
```

With `LIBSTONE_BUILD_TOOLS` and `LIBSTONE_BUILD_TESTS` enabled, the
integration test uses Python 3 and the configured C compiler to generate,
compile, and execute parsers. It can also be run directly:

```sh
python3 cornerstone/Aurocks/tests/integration.py build/aurocks/aurocks cc
```

To build in-tree, enable `LIBSTONE_BUILD_AUROCKS` in the root CMake project.

## CLI

| Option | Effect |
| --- | --- |
| `-h`, `--help` | Print usage. |
| `-V`, `--version` | Print the native generator version. |
| `-e NAME`, `--entrypoint NAME` | Add a public parser entry point with the given name. |
| `-o FILE`, `--output FILE` | Write generated C to a file instead of stdout. Use `-` for stdout. |
| `--check` | Report the selected backend, algorithm, disambiguator, state count, and conflict count instead of writing C. |
| `--backend NAME` | Select a backend. |
| `--algorithm NAME` | Select the parser analysis algorithm. |
| `--disambiguator NAME` | Select the ambiguity resolution policy. |
| `--lua FILE` | Read explicit Lua extension source from the named file. Grammar text does not implicitly load Lua files. |
| `--no-strpool` | Compatibility spelling; the native generator does not emit a string pool. |

Warnings and errors go to stderr. A grammar, compilation, or file error
exits with status 1; incorrect CLI usage exits with status 2. `--check`
does not modify an `-o` destination.

## Grammars and generated parsers

```text
%{
#include <stdlib.h>
static long number;
%}

%start integer
%skip /[[:space:]]+/

integer: /[0-9]+/ {
    number = strtol($TOKEN, NULL, 10);
    free($TOKEN);
    $$ = &number;
};
```

A production has a name, a colon, one or more alternatives separated by
`|`, and a terminating semicolon. Quoted terminals match literal text;
slash-delimited terminals match regular expressions at the current
position. The portable matcher uses a byte-oriented ERE subset; use
`%empty`, not a zero-length terminal, for an empty alternative. `%skip`
declares a layout expression, and `%start` is required to select the
start rule. C between `%{` and `%}` is a prologue; use `%%` after the
productions to attach a C epilogue.

Actions are C fragments in braces. `$$` is the result, `$1`, `$2`, …
are values of symbols in the production, and `$TOKEN` refers to the
matched token text. On the portable backend `$OFFSET` is the production's
starting byte offset, after leading layout. Semantic values use `void *`; an action that
captures a terminal receives a NUL-terminated allocation and can
release it with `aurocks_free` when no longer needed. `$TOKEN` and
the corresponding `$N` share the same allocation. A generated parser
exposes `void *parse_<start>(const char *input)` by default and a
`void *yyparse(const char *input)` compatibility entry point.
`parse_<start>_error()` (and `<entrypoint>_error()` for a custom name)
returns a pointer into the failed input, or `NULL` on success, even when
the semantic result itself is `NULL`. Keep the input alive while using
the pointer. The generated source also includes the 35 legacy
`aurocks_*` allocation, string, conversion, formatting, and boxing helpers.
See [`tests/expression.g`](tests/expression.g)
and [`tests/json.g`](tests/json.g) for executable examples.

## Backends and embedding

| Backend | Algorithms | Generated runtime |
| --- | --- | --- |
| `portable` (default) | `earley` | Standalone C99 unless the grammar uses `%fault` |
| `liblr` | `lr1` (default), `lalr1`, `slr1` | Same runtime as `portable`; liblr supplies LR analysis at generation time |
| `libglr` | `lr1` | Linked C99 using `aurocks/Runtime.h` and the `Aurocks::Runtime` CMake target |

Use `%backend`, `%algorithm` (or `%algo`), and `%disambiguator` to set
grammar defaults. Explicit CLI options override their grammar defaults.
`%expect N` checks the reported LR conflict count. With `portable`, this
count comes from LR(1) analysis, while the state count is the number of
dotted production positions, not the number of runtime chart entries.
The disambiguator names are `first` (default), `error`, `prefer`, and
`priority`. `%prefer` / `%avoid` and `%priority N` / `%dprec N`
annotate production alternatives. `prefer` compares preference before
numeric priority; the other policies compare priority first. `error`
rejects a remaining tie before running any actions. Grammars that can
recurse without consuming input are rejected because they can produce
cyclic parse forests.

The linked backend builds a LibGLR parse forest and evaluates semantic
actions once per occurrence in the selected tree, including repeated
nullable symbols at the same input position. Matching uses LibGLR's
POSIX ERE scanner; libchomsky3 supplies supplemental regex diagnostics.
The linked scanner can offer multiple matching prefixes for a terminal,
while the portable matcher uses the longest prefix only. The linked
scanner does not support all extensions of the portable regex dialect.
After generating
with `aurocks --backend libglr grammar.g -o parser.c`, link the resulting
source against the runtime target:

```cmake
add_executable(my-parser main.c parser.c)
target_link_libraries(my-parser PRIVATE Aurocks::Runtime)
```

For in-process grammar compilation, link `aurocks_compiler`
(`Aurocks::Compiler`). The C++ interface is `aurocks/Compiler.hpp`
(`aurocks::compile`), and the C interface is `aurocks/API.h`
(`aurocks_compile`). The C result owns its generated source, analysis,
and diagnostics; pointers returned by its accessors are borrowed until
`aurocks_result_destroy`.

## Extensions

Supply Lua source at generation time with `--lua FILE` (or as source text
through the embedding API). Grammar text cannot load a script. The
`laurocks` API provides:

- `get_start()` and `production_count()`.
- `production(rule)`, returning a table with `lhs`, `rhs_count`,
  `directive_count`, `line`, and `column`.
- `symbol(rule, index)`, returning the symbol kind and text.
- `production_directive(rule, index)`, returning the directive name
  (without `%`) and its raw argument text (an empty string if absent).
  This exposes source annotations such as `%prefer` and `%dprec`, not
  semantic action code or scores staged by the script.
- `warn(message)` and `select_disambiguator(policy)`. Policy selection
  cannot contradict an explicit host option and is unavailable in Lua
  scoring mode.

To change the grammar or supply production scores, declare the corresponding
extension:

- `%rewriter lua` enables edits through `laurocks.set_start(name)` and
  `laurocks.replace_literal(rule, symbol, text)` or
  `laurocks.replace_nonterminal(rule, symbol, name)`. Replacements keep
  the original symbol kind and production arity, so action references
  keep their positions. A new start or nonterminal reference must name
  an existing production. `get_start` and `symbol` see staged edits.
- `%disambiguator lua` (or `--disambiguator lua`) enables
  `laurocks.set_rule_priority(rule, score)`. The script must assign at least
  one signed integer score and cannot replace an existing `%priority` or
  `%dprec` annotation. Scores become priorities in the generated C; Lua
  does not run in the generated parser.

Indices are one-based and address production alternatives in grammar
order. At most 16 distinct symbol replacements and 256 distinct rule
scores may be staged per invocation. Repeated assignments to the same
target keep the last value. Edits, scores, and warnings from a failed
script are discarded. Lua execution is bounded and has no file I/O or
module loading.
The sandbox applies only to Lua. C prologues, actions, and epilogues are
emitted as native code; only compile and run generated parsers from
grammars you trust.

## Runtime fault rules

Declare `%fault` (or `%Fault`) at grammar scope. Rules run **only in the
generated parser**, on a syntax failure or rejected ambiguity, before
semantic actions run. The first matching rule wins. The generator checks
declaration syntax and PEG validity; rules cannot customize build diagnostics.

```text
%start statement
%fault "','" => silent_patch ";"
%fault "!." => loud_patch ";" "inserted a missing semicolon"
%fault "'?' / '!'" => inform "expected a statement terminator"
%fault "[a-z]+" => warn "unexpected word" using describe_fault
%fault "." => error "unexpected input"

statement: "quit" ";";
```

The quoted pattern is a Firestone liblr **byte PEG**, anchored at the
parser's error offset. It supports single- or double-quoted literals,
classes such as `[a-z]` and `[^a-z]`, `.`, sequence, ordered choice `/`,
groups, lookahead `&` and `!`, and suffixes `?`, `*`, and `+`. `!.`
matches true EOF without consuming input; `''` matches empty text anywhere.
The directive's outer string has its own escaping, so write `\\xFF` there
to pass `\xFF` to the PEG.

| Disposition | Runtime behavior |
| --- | --- |
| `silent_patch "replacement"` | Replace the matched bytes and retry without a message. An empty replacement deletes the match. |
| `loud_patch "replacement" ["message"]` | Patch and retry, reporting the edit on stderr. `load_patch`, `load patch`, and `loud patch` are aliases; `silent patch` is also accepted. |
| `inform "message"` | Report an informational diagnostic on stderr; the parse still fails. |
| `warn "message"` | Report a warning on stderr; the parse still fails. |
| `error "message"` | Report an error on stderr and return failure. |

Any rule can end in `using handler_name`. Define the callback in the C
epilogue; it receives the live parser state and can change the message or
the patch span and replacement. For example:

```c
static void describe_fault(const aurocks_fault_state *state,
                           aurocks_fault_response *response) {
    if (state->reason == AUROCKS_FAULT_AMBIGUITY)
        response->message = "more than one parse fits this input";
}
```

`state` exposes the current and original inputs, byte offsets, match end,
retry number, failure reason, and backend. For `portable` and `liblr`,
cast `native_parser` to `const aurocks_parser *` in the epilogue to inspect
the generated Earley chart, items, and forest edges. For `libglr`, include
`<glr/parser.h>` and cast to `const glr_parser_t *` to inspect the native
grammar, parse table, and forest. The `frontier` field also exposes LibGLR
stack snapshots at the farthest visited token boundary, when available
(up to 256 stacks, bounded by one million copied stack entries per attempt;
`truncated` flags incomplete snapshots). All parser state is read-only and
borrowed during the callback. Response strings must outlive the callback.

Recovery is limited to eight edits, with progress, input size, and PEG
work checks. Patches affect only a private copy; final error pointers still
point into the caller's original input, and successful semantic actions run
once on the accepted parse. Failures caused by resource exhaustion or
semantic evaluation do not invoke fault rules.

Grammars with fault rules still emit C99, but must link `Aurocks::FaultRuntime`
(plus `Aurocks::Runtime` if using `libglr`):

```cmake
add_executable(my-parser main.c parser.c)
target_link_libraries(my-parser PRIVATE Aurocks::FaultRuntime)
```

## Migrating from `aurocks.pl` 2.0

The native tool accepts the legacy grammar sections and action references
covered above, but does not reproduce every property of the Perl
generator or its emitted C API.

- Remove `--strpool` and `--poolfile`; neither string-pool sidecars nor
  the `strpool_*` runtime API are available. `--no-strpool` is accepted
  for existing invocations.
- Remove `--liblr-helper`. The native generator does not invoke an
  external matcher helper. `--backend liblr` selects LR grammar analysis;
  it is not a replacement for the helper's regex-to-DFA conversion.
- Port actions and epilogues that depend on old private parser internals
  such as `attempt`, `GLRParser`, or `glr_<production>`. The public entry
  points and runtime helpers remain available.
- Directives outside the supported set, such as `%left`, `%merge`,
  `%glr`, and `%sppf`, do not configure the native runtime. They are
  retained as metadata and can produce an `unsupported-directive`
  warning.
