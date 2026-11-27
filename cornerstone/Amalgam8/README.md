# Amalgam8

Amalgam8 compiles an S-expression DSL into a header-only C API or a C++ source
file containing that API and `main`. Use it to generate libraries, share
compile-time vocabulary between modules, or build a small language that has
the same implementation behind its library and command-line interfaces.

## Build and use

```sh
cmake -S cornerstone/Amalgam8 -B build/amalgam8 -DAMALGAM8_BUILD_TESTS=ON
cmake --build build/amalgam8 --parallel
ctest --test-dir build/amalgam8 --output-on-failure
```

The compiler uses C++20 and the checked-in Firestone `LibDSL.hpp`. The
executable uses POSIX file publication. The consumer tests also need a C11
compiler, a shell, and Python 3.8+. The top-level equivalent is
`-DLIBSTONE_BUILD_AMALGAM8=ON`; build the `amalgam8` target. Use
`-DAMALGAM8_BUILD_TOOLS=OFF` for just the embeddable target.

From the repository root, after building:

```sh
mkdir -p build/amalgam8-demo
cp cornerstone/Amalgam8/examples/Minimal.am8 cornerstone/Amalgam8/examples/Generator.am8 build/amalgam8-demo/
build/amalgam8/amalgam8 build/amalgam8-demo/Minimal.am8
cc -std=c11 -I build/amalgam8-demo -x c -o build/amalgam8-demo/check - <<'EOF'
#include "Minimal.hpp"
int main(void) { return minimal_answer(2) != 42; }
EOF
build/amalgam8-demo/check
build/amalgam8/amalgam8 --cli build/amalgam8-demo/Generator.am8
c++ -std=c++11 build/amalgam8-demo/Generator.cpp -o build/amalgam8-demo/generator
build/amalgam8-demo/generator x
```

`amalgam8 Foo.am8` and `amalgam8 --api Foo.am8` both produce `Foo.hpp`
beside `Foo.am8`, byte for byte. `--cli` writes `Foo.cpp` beside the input
and does not read or require `Foo.hpp`. For `(cli ENTRY)`, `ENTRY` must
have signature `int (int, char **)`; `main` forwards both arguments and
returns the result. The legacy `(cli "C++ body")` can use `argc` and `argv`
directly. CLI mode without an entry is an error.

The driver exits `0` on success (including `--help`), `2` for command-line
usage errors, and `1` for compilation or publication errors. Diagnostics
go to stderr. Output is published by renaming a completed temporary sibling;
a failed compilation or rename does not overwrite the previous output. No
timestamps or compiler-side workspace paths are written to generated source.

## Grammar

One `(library NAME ...)` or `(module NAME ...)` per file. Parentheses
form lists; whitespace separates tokens; `;` starts a line comment.
Double-quoted atoms support `\"`, `\\`, `\n`, `\r`, and `\t`.
Diagnostic spans are half-open byte intervals with one-based lines and byte
columns. Names that become C identifiers must be valid in both C and C++.

| Top-level form | Purpose |
| --- | --- |
| `(import "path.am8" [Alias])`, `(export NAME ...)` | Share direct module exports |
| `(syntax PATTERN TEMPLATE)` | Register a structural syntax rewrite |
| `(stage EXPR)`, `(stage-bind NAME EXPR)`, `(stage-fn NAME (PARAM ...) EXPR)` | Evaluate compile-time code |
| `(declarations DECL ...)` | Produce an ordered declaration group, possibly empty |
| `(native NAME)` | Enable a callback registered by the embedding host for this module |
| `(include HEADER)`, `(use FAMILY PATH)`, `(use FAMILY:NAME)` | Declare consumer dependencies |
| `(def NAME EXPR)` | Define a parenthesized object-like macro; legacy quoted text and unresolved bare C identifiers are opaque |
| `(type NAME TYPE)` | Define a type alias |
| `(record NAME ((TYPE FIELD) ...))` | Define a C struct and its typedef |
| `(enum NAME (ITEM ...))` | Define a C enum and typedef; an item is `NAME` or `(NAME EXPR)` |
| `(fn TYPE NAME ((TYPE PARAM) ...) STMT)` | Define a function; a quoted body is the legacy raw form |
| `(extern TYPE NAME ((TYPE PARAM) ...))` | Declare a C-linkage external function |
| `(var TYPE NAME EXPR)` | Define a static object with a constant initializer |
| `(c "source")`, `(cpp "source")`, `(cpp-type NAME)` | Emit opaque C/C++ or mark a type as C++-only |
| `(cli ENTRY)` or `(cli "body")` | Select the root entry point |

Structured types include C scalar and named types, `(ptr TYPE)`,
`(const TYPE)`, `(volatile TYPE)`, `(array TYPE LENGTH)`, and
`(function RESULT (TYPE ...))`. Use `(ptr (function ...))` for a function
pointer. Quote compound scalar spellings like `"unsigned long"`; legacy
simple pointer spellings like `"const char *"` are also accepted. Structured
uses of `bool`, `size_t`/`ptrdiff_t`, and the fixed-width integer aliases
automatically request the relevant C standard headers. Declarations buried in
opaque text need explicit includes.

Statements: `(block STMT ...)`, `(let TYPE NAME [EXPR])`, `(set TARGET EXPR)`,
`(if EXPR STMT [ELSE])`, `(while EXPR STMT)`, `(return [EXPR])`,
`(expr EXPR)`, `(raw "C statement text")`. Expressions include names, numeric
and quoted string literals, `(string "text")`, `(call NAME ARG ...)`,
`(field EXPR NAME)`, `(ptr-field EXPR NAME)`, `(index EXPR INDEX)`,
and `(raw "C expression text")`. C arithmetic, comparison, logical and
bitwise operators use prefix notation, e.g. `(+ x (* y 2))`, `(! done)`,
`(& value)` and `(* ptr)`. Structured expressions preserve grouping and C
short-circuit semantics; they do not impose a new operand evaluation order.

## Modules and staging

Imports are relative to the importing file. The compiler uses canonical file
identity to avoid reloading a module through another spelling and to detect
cycles. An unaliased import brings its explicit exports into scope; an aliased
import exposes `Alias.name` only. Imports are direct, not transitive
re-exports. A reference that is visible from more than one origin is ambiguous,
including a local name colliding with an unqualified imported name. A qualifier
can disambiguate a reference. Imported private names and colliding imported
structured exports are renamed. Explicit external ABI names cannot be renamed
and conflicting external declarations are diagnosed. Quoted raw source has
no DSL lookup or renaming; declare public C names explicitly.

The compiler loads the complete module graph before expansion. It registers
literal top-level `syntax` and `native` forms before processing the remaining
forms in dependency order and then source order. `stage-bind` and `stage-fn`
are lexical: their definitions see earlier stage definitions from their
module and explicit exports of direct imports. A named stage function may
call itself. Stage output is expanded again before lowering, but cannot
register imports, macros or native callbacks.

* `(syntax (repeat $x) (+ $x $x))` captures syntax trees. A repeated capture in
  a pattern must match structurally. The first matching rule within a module
  wins. Captured syntax keeps caller scope; names from the template resolve
  from the definition module. Template `(fresh label)` introduces one fresh
  name for that label per rewrite. Insert caller names explicitly via `$name`.
* Stage values are syntax (including number, boolean and string atoms) or
  lexical callables, constructed by `(lambda (PARAM ...) EXPR)` or
  `stage-fn`. Stages provide `let` (with optional list destructuring), boolean
  `if`, `equal?`, `list`, `quote`, and function application. Bare
  identifiers are stage references, not literal data.
* `(quote TREE)` yields opaque data. `(quasiquote TREE)` builds a template
  with `(splice EXPR)` replacing one syntax node and `(splice-list EXPR)`
  inserting the elements of a syntax list. Quasiquotation also supports
  `(fresh label)`. A callable cannot be spliced or emitted as a declaration.
  `(stage EXPR)` must produce a declaration or `(declarations DECL ...)`;
  there is no implicit flattening of arbitrary lists into declarations.

Stage failures and syntax expanded from templates retain source locations and
expansion notes. All modules share finite input, expansion work, rewrite,
generated-node, and stage-step budgets. See [`Contracts.hpp`](Contracts.hpp)
for the default limits and how an embedder overrides them.

## Generated linkage and trusted escapes

Functions are `static inline`; functions and mutable `var` storage have
translation-unit-local identity. In particular, including the same header
in two translation units creates **two** independent copies of each mutable
object. Use caller-owned storage or an explicit external implementation
when identity must be shared. A `def` has no storage or address of its own.
An `extern` declaration does not provide an implementation.

Typed declarations are ordered from semantic dependencies, with forward
declarations for supported recursion. Structured code cannot use a
C++-only type in a C declaration. C++ dependency and `cpp` sections are
guarded by `__cplusplus`. API guard names start with `AMALGAM8_` followed by
the library name; use distinct library names when including several APIs in
one program. `_HPP`, `_CLI`, and `_UNUSED` are reserved suffixes on the
library's generated `AMALGAM8_` prefix.

Raw C/C++ and quoted legacy bodies are **trusted source**. They are not parsed
as C/C++, and their references, linkage, initializer behavior and conditional
compilation are the author's responsibility. Do not define another `main` in
a raw CLI compilation. Native callbacks likewise run in the embedding
process; declared capabilities document trust, not a sandbox.

## Embedding and libraries

Link `Amalgam8::compiler` and `#include "Amalgam8Base.hpp"`. The compiler has no
process entry point and never publishes files itself. Call `am8::compile_file`
or `am8::compile_source`; the result contains `source`, a dependency list,
and the locations of opaque fragments.
`am8::Options::mode` defaults to `Mode::api`. With CMake installation, consume
the same target via `find_package(Amalgam8 CONFIG REQUIRED)`.

`Options::native.add(NativeForm{...})` registers a form name, arity range,
optional validator, lowering callback, and capability descriptions. A
source module activates it with `(native NAME)`. The callback receives
located syntax after macro normalization and returns a vector of declarations
for ordinary expansion and checking. See the
[native extension example](examples/native/README.md) and the
[language example](examples/language/README.md).

`--deps` prints resolved system, external, and bundled dependencies after
successful publication. It flags where opaque fragments prevent inference;
declare their actual requirements with `include` or `use`. Explicit headers
and native prerequisites are emitted before generated declarations.
`--bundle` inlines only reviewed redistributable snapshots.
Use `--include-root DIR` to point at the Libstone source root
when not running from it. See the [dependency catalog](docs/catalog.md) for
facility spellings, targets, and packaging limits, and
[verification](docs/verification.md) for tested platforms and gaps.
