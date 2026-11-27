# LibDSL (C++20)

`libdsl/LibDSL.hpp` is a standalone header. Copy it into a C++20 project and
`#include "LibDSL.hpp"`, or link the CMake interface target `Firestone::dsl`.
The latter is deliberately separate from the C++17 Firestone component
aggregate. No TTyPack header, generator, or library is needed by LibDSL.

To run the LibDSL tests from the source tree:

```sh
cmake -S firestone -B build/firestone-dsl -DFIRESTONE_BUILD_TESTS=ON
cmake --build build/firestone-dsl --target firestone_dsl_tests --parallel
ctest --test-dir build/firestone-dsl -R '^firestone\.dsl\.' --output-on-failure
```

The extension namespaces are `dsl::live` (source positions, diagnostics and
interactive syntax), `dsl::terminal` (compiled Terminfo, escape operations and
styled text), `dsl::cli` (command schemas), `dsl::repl` (interactive sessions and
adapters), and `dsl::synth` (language specification and execution). The
`Terminfo`, `GlobalEscapeCodes`, `OptParser`, `REPL` and `LanguageSynthesizer` tags can be added
to `dsl::DSL<Derived, ...>`; direct users need not inherit from the CRTP base.

## Public entry points

| Faculty | Native and textual entry points |
| --- | --- |
| Shared services | `live::SourceManager`, `OriginTable`, `Storage`, `Diagnostic`, `CallbackRegistry` |
| Interactive parsing | `live::analyze_peg`, `syntax_grammar`, `analyze_cfg`, `SyntaxPrefixCache` |
| Terminal | `terminal::terminfo::load_from` for compiled bytes; `load` for discovery; `tparm` and `padding` for named capability programs; `terminal::InputDecoder`, `RawMode`, `Backend` |
| Escape and style | `terminal::escape::Sequence` and `terminal::StyledText`; native builders and `parse`; `terminal::RenderMode` controls rendering |
| Command line | `cli::OptParser` over native `cli::Command` or `from_text`; `parse`, `dispatch`, `help`, `complete` |
| Interactive tools | `repl::Session` with `dsl::IO` (`run_lines` / `run`), or `run_terminal`; `repl::Analysis` with an `AnalysisConfig`; `repl::Directives` shares OptParser schemas |
| Language synthesis | `synth::Specification` / `SpecBuilder` / `MetaLanguage`; `MetaEvaluator` for lexical closures; `AttributeEngine` for dependency-checked analysis; `Executable` and `ExecutionContract` for native state and executable semantics |
| Reviewed optimization | `synth::GuardedTermOptimizer`, `reviewed_ast_candidate`, `reviewed_graph_reduce`, and `reviewed_equality_saturate`; the reviewed adapters require an explicit domain and certification and candidate-check callbacks |

For example, these produce the same plain text, but the textual style can also
be rendered as ANSI, 256-color, true color, grayscale, monochrome, or debug:

```cpp
dsl::terminal::Style red;
red.foreground = dsl::terminal::Color{255, 0, 0};
dsl::terminal::StyledText native;
native.span("hello", red);
auto textual = dsl::terminal::escape::Sequence::parse(
    R"((style (fg 255 0 0) (text "hello")))");
assert(native.render() == textual.render());
```

`OptParser::parse` expects arguments **after** `argv[0]`. In a short cluster
the first option requiring a value consumes either the remainder of that
token or the next token. `--` ends option interpretation.

## Examples

Configure with `-DFIRESTONE_BUILD_DSL_EXAMPLES=ON`, then build
`firestone_dsl_archive_cli` and `firestone_dsl_live_lisp`. The same sources can
be compiled using a C++20 compiler and `-I firestone/libdsl`.

```sh
./firestone_dsl_archive_cli files run -xvf archive.tar member
./firestone_dsl_archive_cli files run --render grayscale --help
./firestone_dsl_live_lisp
```

The Lisp demo uses a synthesized specification for structural macros and an
evaluator with lexical closures. At the prompt enter, for example,
`(((lambda (x) (lambda (y) (list x y))) (quote one)) (quote two))`; the
result is `(one two)`. The demo also accepts piped input. Enter `:` for directive
help. To enable `:script`, start with `--script` pointing to the **absolute**
path of [`lisp-hook.sh`](../examples/libdsl/lisp-hook.sh), or to another
executable you trust. A separate
[`reference.hpp`](../examples/libdsl/reference.hpp) shows a checked arithmetic
language, an imperative language with Hoare contracts, and a closure-capable
Lisp with user-defined machine state and scope resolution.
In the compiled Lisp reference, `(context-value)` reads a host value
through a custom scoping metaprocedure rather than through the caller's Lisp
bindings.

## Input and trust boundaries

- Source positions use half-open byte ranges. A byte offset is not a terminal
  display column. Preserve the original input when indexing a diagnostic.
- `dsl::parse_bounded_sexpr(source, dsl::SExprLimits{})` is the bounded reader for new
  textual specifications. The original one-argument reader remains available.
  A textual specification describes data; native callbacks still come from the
  embedding application.
- Ordinary terminal text is data, not an escape-code program. Only send trusted
  programs to an API specifically intended to emit control operations; use plain
  or debug rendering for output that isn't destined for a terminal.
- An interactive analysis callback is distinct from evaluation. Keep completion,
  syntax highlighting, and indentation free of language-state effects. Native
  callbacks run in-process and are trusted; declaring an equation pure does not
  sandbox its C++ implementation.
- Enable scripts only for explicitly trusted executables. Their interpreter, if
  any, is selected by their shebang. Environment request variables are a one-way
  input channel, not a way to update the parent environment.
- Runtime verification of a precondition, postcondition, or frame is evidence
  for one execution, not a formal proof of partial or total correctness.
- Define `LIBDSL_LIVE_POSIX=0` consistently in all translation units to disable
  POSIX functionality. Terminfo file parsing and the portable IRs do not require
  a terminal device.

The script protocol uses `(libdsl-script 1 request ID OP "payload")` and
`(libdsl-script 1 ok ID "payload")` (or `error` instead of `ok`). Payloads
are quoted S-expression atoms. Replies with a different ID, unsupported
protocol version, or trailing forms are errors. Environment transport passes
only the request to the child; the reply still travels on standard output.
The dedicated-pipe and shared-memory transports are Linux-specific. On older
systems using the script runner may require the platform's pthread or
real-time library; the portable facilities do not require either.

`live::Storage` scopes only the allocations made through its PMR resource.
Borrowed storage must outlive its users. Source buffers and provenance records
can use the selected resource, but diagnostic strings and callable lifetimes
can deliberately use ordinary owning storage. Selecting a fixed PMR resource
is not a process-wide no-allocation policy.

Pass `live::Storage` to `repl::Editor(limits, storage)` or as the last
argument to `repl::Session(analysis, evaluator, limits, storage)`. It backs
editable text, undo/redo snapshots, the kill ring, history drafts, and session
input scratch buffers. Copies share the source storage; assignment retains
the destination storage. Stored history keeps its existing container type
and uses ordinary allocation, as do callbacks and public results. A monotonic
resource does not reclaim individual edits.

## REPL keymaps

`Session::set_key_style` selects an Emacs or Vi subset. These bindings
supplement the shared arrows, Home/End, Tab completion, Enter submission,
Alt+Enter newline, Ctrl+C interrupt, and Ctrl+D delete/EOF.

| Style | Keys | Action |
| --- | --- | --- |
| Emacs | Alt+F / Alt+B | Move to the end of the current or next word run / start of the current or previous word run |
| Emacs | Alt+D / Alt+Backspace | Kill forward / backward using those motions |
| Emacs | Alt+`<` / Alt+`>` | Move to the start / end of the input buffer |
| Emacs | Ctrl+G | Clear the mark without discarding the buffer |
| Vi insert | Ctrl+W | Kill backward by a word run |
| Vi normal | `w` / `b` | Move to the next word-run start / current or previous word-run start |
| Vi normal | `dw` / `db` | Kill the span covered by `w` / `b` |
| Vi normal | `^`, `I`, `A` | Move past leading spaces and tabs; insert there; or insert at line end, respectively |
| Vi normal | `gg` / `G` | Move to the start / end of the input buffer |
| Vi normal | `X`, `Y` | Delete backward by one glyph; copy the current line (like `yy`) |

Word motions step over the editor's glyph boundaries. Runs distinguish ASCII
whitespace, word characters (ASCII letters, digits, underscore, and non-ASCII
glyphs), and other characters. This is not Unicode word segmentation. Motions
can cross lines; a word kill uses the active selection if one exists. Vi
normal mode retains the editor's insertion-point cursor and shared kill ring;
it does not implement counts, a general operator-motion grammar, or separate
Vi registers.

## Scope and limits

- Terminfo discovery searches compiled directory trees; it does not read
  hashed databases or actively probe a terminal. Direct byte parsing works
  without a terminal or database. Unicode display widths are estimates, not
  measurements from the user's terminal font.
- The editor has Emacs and Vi subsets, not drop-in emulations. It bounds
  input, history, undo, and kill storage by policy. Evaluator cancellation is
  cooperative.
- The prefix cache memoizes bounded analyses; it does not incrementally
  repair Earley charts after arbitrary edits. Recovered syntax is not
  permission to evaluate malformed or incomplete input.
- Macro templates use `(fresh NAME)` for introduced hygienic bindings.
  Other names follow the object language's rules. `synth::LinkedSpecification`
  links named expression, statement, and attribute declarations from native
  and textual specifications through one path. Hosts register trusted
  executable form factories and attribute callbacks; textual specifications
  cannot introduce new native capabilities. No SMT solver or automatic proof discharge is
  included. `ExecutionContract::require_exceptional` checks the exceptional
  outcome before rollback. `run_transactional` uses the caller's `noexcept`
  restore callback and cannot undo external effects. Ordinary `run` does
  not roll back state. Formal obligations and externally reported evidence
  remain distinct from runtime checks.

## Derivation and licensing

Terminal support is derived from the vendored [TTyPack](../../third_party/ttypack)
sources, not an adapter that includes or links TTyPack. The applicable TTyPack
MIT notice is retained with the derived code. The implementation records the
specific source files and notices for derived capability, escape, and character
inventories in the header. Development coordination materials in
[`libdsl/swarm`](../libdsl/swarm) are not installed or required to build or use
LibDSL.
