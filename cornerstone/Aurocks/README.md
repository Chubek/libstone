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
matched token text. Semantic values use `void *`; an action that
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
| `portable` (default) | `earley` | Standalone C99 |
| `liblr` | `lr1` (default), `lalr1`, `slr1` | Same standalone runtime as `portable`; liblr supplies generation-time analysis only |
| `libglr` | `lr1` | Linked C99 using `aurocks/Runtime.h` and the `Aurocks::Runtime` CMake target |

Use `%backend`, `%algorithm` (or `%algo`), and `%disambiguator` to set
grammar defaults. Explicit CLI options override their grammar defaults.
`%expect N` checks the reported LR conflict count. With `portable`, this
count comes from LR(1) analysis, while the state count is the number of
dotted production positions, not the number of runtime chart entries.
The disambiguator names are `first` (default), `error`, `prefer`, and
`priority`. `%prefer` / `%avoid` and `%priority N` / `%dprec N`
annotate production alternatives.

The linked backend builds a LibGLR parse forest and evaluates semantic
actions after selecting a derivation. Matching uses LibGLR's POSIX ERE
scanner; libchomsky3 supplies supplemental regex diagnostics. It does
not support all extensions of the portable regex dialect. After generating
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
through the embedding API). Scripts can inspect the grammar with
`laurocks.get_start`, `production_count`, `production`, and `symbol`, add
warnings with `laurocks.warn`, or choose a built-in policy with
`laurocks.select_disambiguator` unless it contradicts an explicit host
option.

- `%rewriter lua` enables edits through `laurocks.set_start(name)` and
  `laurocks.replace_literal(rule, symbol, text)`. For example,
  `laurocks.replace_literal(1, 1, "new")` replaces the first symbol of the
  first production, if it is a literal.
- `%disambiguator lua` (or `--disambiguator lua`) enables
  `laurocks.set_rule_priority(rule, score)`. The script must assign a score
  to at least one production. Scores become priorities in the generated
  C; Lua is not used by the generated parser.

Indices are one-based and address production alternatives in grammar
order. Lua execution is bounded and has no file I/O or module loading.
The sandbox applies only to Lua. C prologues, actions, and epilogues are
emitted as native code; only compile and run generated parsers from
grammars you trust.

`%fault CODE ["message prefix"] => "replacement"` customizes a
generator diagnostic's message; the first matching declaration wins.
It does not change the diagnostic code, severity, or source position,
and it does not affect generated-parser errors.

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
