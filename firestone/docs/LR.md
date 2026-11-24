# Lexing and parsing

`Firestone::lr` is a header-only C++17 component. It uses no external lexer,
parser generator, or runtime. Include `<liblr/LR.hpp>` for the complete
component or include the individual headers you need. The APIs are in
`firestone::lr`.

```cmake
find_package(Firestone 0.1 CONFIG REQUIRED COMPONENTS lr)
target_link_libraries(my_frontend PRIVATE Firestone::lr)
```

## Choosing an API

| Need | Headers |
| --- | --- |
| Build, run, or minimize a finite automaton | `NFA.hpp`, `DFA.hpp`, `ThompsonConstruction.hpp`, `DFAMinimization.hpp` |
| Validate or decode UTF-8 | `UTF8Automaton.hpp` |
| Build a lexer from ordered rules | `LexerGenerator.hpp` |
| Run or export a DFA scanner | `DFA2SIMD.hpp`, `DFA2C99.hpp`, `DFA2Assembly.hpp` |
| Define a context-free grammar | `EBNF.hpp` |
| Generate a bottom-up table or parse with one | `ParserGenerator.hpp`, `LALR.hpp`, `GLR.hpp` |
| Predictive or generalized top-down parsing | `LLTableDriven.hpp`, `LLFunctionDriven.hpp`, `GLL.hpp`, `ATN.hpp` |
| Write a parser directly instead of building a CFG | `RecursiveDescent.hpp`, `ParserCombinator.hpp`, `PEG.hpp` |
| Diagnose and recover from recognized errors | `DiagnosticsEngine.hpp` |

## Grammar syntax

`readEBNF(text)` lowers text to the same `Grammar` used by the programmatic
API. For example, `sum = 'word' { '+' 'word' };` describes a list of words
separated by `+`. Bare names refer to nonterminals and single- or
double-quoted text names a terminal token, not a lexer pattern.

- `=` or `::=` starts a rule body, and `;` terminates it. Use `|` for
  alternatives and an empty alternative for epsilon.
- `(...)` groups, `[...]` is optional, and `{...}` repeats zero or more
  times. Postfix `?`, `*`, and `+` also work.
- `#` starts a line comment. `%start rule;` overrides the default start
  symbol, the first rule in the source.
- Quoted terminals support `\n`, `\r`, `\t`, `\0`, `\xHH`, and escaped quotes
  and backslashes. These escapes name exact bytes, including embedded NUL.

Generated rules for groups and repetitions have ordinary grammar symbols
and can appear in parse trees. Avoid repetitions of nullable expressions
when a unique derivation is required.

## Byte lexers

`compile_regex` builds a Thompson NFA. Use `determinize` for a DFA that
supports `match_prefix`, and `minimize` to remove unreachable states and
combine equivalent states while preserving acceptance metadata. DFA
matching may return a zero-length match for a nullable regex.
`determinize_bounded(nfa, max_states)` limits the number of reachable DFA
states and throws `std::length_error` instead of returning a partial result.
Use `NFA::add_alternative` or `add_regex_alternative` to combine independently
built alternatives while preserving acceptance metadata and existing state IDs.

Patterns support literals, grouping, alternation, concatenation, postfix `?`,
`*`, `+`, `.`, and byte classes with ranges and optional negation (`[^...]`).
`.` matches any byte; there is no Unicode mode, anchor, or counted repetition.
Use `\n`, `\r`, `\t`, `\0`, `\xHH`, or a backslash before regex punctuation
for escapes. Invalid patterns report a `RegexError` with a byte offset.

`make_lexer` (or `LexerGenerator::build`) combines ordered `LexerRule`s and
minimizes the DFA by default. A rule is `{pattern, token_id, skip}`; `skip`
defaults to false. Rules must consume at least one byte. `Lexer::next` takes
the input and an in/out byte cursor. The longest match wins, with rule order
breaking ties. Skipped matches are consumed internally. An unknown byte
returns `LexStatus::Error` and advances by one, so a caller can report it and
continue. `LexStatus::End` leaves the cursor at the end.
Use `LexerRule::literal(bytes, token_id)` for fixed spellings that should not
be interpreted as regexes.

`SIMDMatcher` and `SIMDLexer` use 128-bit lanes for runs that stay in the
same DFA state on supported GCC/Clang x86-64 and AArch64 targets. Other
transitions and unsupported targets use equivalent scalar matching. The
vector path can also be disabled explicitly when constructing either wrapper.

`emitC99` emits a standalone C99 translation unit for 8-bit-byte targets.
`emitAssembly` emits GNU-style x86-64 System V LP64 assembly for ELF targets,
with a limit of 65,535 DFA states. Both use `DFA::byte_classes()` to share
identical transition columns when that reduces table size. Both emit a scanner
with the same C ABI:

```c
int scan(const unsigned char *input, size_t size, size_t *cursor,
         size_t *token, size_t *begin, size_t *length);
```

Pass your chosen external symbol name to the emitter. The scanner returns
`1` for a token, `0` for end, `-1` for an unknown byte, and `-2` for invalid
arguments such as a null output or a cursor past the end. It makes no writes
on `-2`. Provide four distinct writable `size_t` objects disjoint from the
input, and a readable input of `size` bytes (`NULL` is valid only at size 0).

### UTF-8

Use `validate_utf8(input)` to check a whole buffer and `utf8_error_offset(input)`
to find the first invalid byte (or the start of an unfinished scalar at EOF).
`decode_utf8_prefix(input)` returns the first scalar and its encoded byte length,
or `std::nullopt` for an empty, malformed, or unfinished prefix.

For streaming input, retain a `UTF8Automaton` across chunks and pass each byte to
`push`. `Complete` means the input ends on a scalar boundary, and `code_point()`
gives the scalar just completed. `Incomplete` needs more bytes. `Invalid` is
sticky until `reset()`. An empty stream is complete but has no scalar.

The automaton rejects overlong encodings, surrogate code points, and values above
U+10FFFF. It accepts embedded NUL, a BOM, and Unicode noncharacters as scalar
values. `utf8_scalar_dfa({token_id})` creates a byte DFA that matches exactly one
scalar, for example `Lexer(utf8_scalar_dfa({1}))`. Use
`utf8_scalar_nfa({token_id, priority})` to add the same scalar recognizer as
an alternative to a byte-regex NFA. This does not change how the byte regex
engine interprets patterns.

## Token parsers

- `generateLRTable` builds canonical LR(1) tables by default and also supports
  `LRMode::lalr1` and `LRMode::slr1`. SLR uses FOLLOW sets and ignores
  unreachable or unproductive productions without renumbering them. Inspect
  `actionRow`, `gotoRow`, and `conflicts` to examine the table.
  `LRParser` defaults to LALR(1), requires a conflict-free table, and supports
  recognition with `accepts` or tree construction with `parse`. It does not
  use precedence to resolve conflicts.
- `GLRParser` defaults to canonical LR(1) and pursues all viable actions
  on a conflict. Use `accepts` to recognize input without packing alternatives,
  or `parse` to obtain a packed `ParseForest`. Nodes share spans, and their
  alternatives reference production IDs and child node IDs.
  Recursive derivations for the same span can form cycles, so traverse
  the forest with a visited set, or call `ParseForest::tree()` for one finite
  derivation.
- `LLTable` computes nullable, FIRST, and FOLLOW sets and reports LL(1)
  conflicts and left recursion. `LLTableDriven` uses explicit stack
  frames; `LLFunctionDriven` uses rule callables to choose productions and an
  explicit stack to expand them. Both require an LL(1) grammar and return a tree
  on success; `LLFunctionDriven::accepts` skips tree construction.
  `LLTableDriven::parseWithDiagnostics` returns a tree or an `LLSyntaxError`
  with the failing token position, the actual symbol, and expected symbols.
- `GLL` and `ATN` use generalized top-down call/return processing,
  including for left recursion and nullable cycles. Both offer
  `accepts` and `parse`; the latter returns one finite derivation, not
  the full forest. `ATN` exposes its network, including rule entries and exits
  and production entries, but has no semantic-action or register layer.
- `PEG` uses ordered choice by default and supports `PEGExpression`
  rule overrides for predicates, greedy repetition, and other operators.
  It rejects direct and indirect left recursion at construction, including
  through nullable prefixes, and reports successful zero-width repetition
  at parse time. `GLL` and `PEG` also support `matchPrefixAt(tokens, begin)` and
  `parsePrefix(tokens, begin)` with absolute token offsets. GLL chooses the
  longest start-rule match; PEG keeps the result of ordered evaluation.
- For hand-written parsers, `RecursiveDescent::consumeSequence` consumes a
  sequence atomically and `lookahead` restores the cursor after the probe.
  `ParserCombinator<T>::flatMap` chooses a parser from a previous result, and
  `repeat(minimum, maximum)` matches a bounded number of consuming results.

## Connecting lexing and parsing

The byte lexer and token parsers are independent. Assign each lexer rule
the numeric ID of its corresponding grammar terminal, or translate the
lexer’s IDs before parsing. Whitespace and comments can be skipped in the
lexer. Keep the lexer’s byte spans if you want to map parser diagnostics
back to source text.
When lexer IDs already match the grammar, `lexSymbols` returns either the token
vector or the first lexical error with its byte span. It does not retain spans
for successful tokens; use `Lexer::next` directly if you need them.

`Grammar` assigns `Symbol` IDs local to that grammar. Construct symbols
with `terminal` and `nonterminal`, select the start with `setStart`, and
define rules with `addProduction`. An empty right-hand side represents
epsilon. The reserved `grammar.eof()` is internal to the token parsers:
never include it in rules or in the input token vector. A CFG `ParseNode`
uses half-open token spans (`begin`, `end`), not source-byte offsets. Its
terminal leaves span one token; an epsilon reduction spans zero.
The token parser APIs reject invalid symbol IDs with `std::invalid_argument`.

For example, this grammar accepts words separated by plus signs:

```cpp
#include <liblr/LR.hpp>
#include <variant>
#include <vector>

int main() {
    using namespace firestone::lr;
    const auto grammar = readEBNF("sum = 'word' { '+' 'word' };");
    const auto word = *grammar.findTerminal("word");
    const auto plus = *grammar.findTerminal("+");
    const auto lexer = make_lexer({{"[a-z]+", word}, LexerRule::literal("+", plus), {"[ \t]+", 0, true}});
    const auto lexed = lexSymbols(lexer, grammar, "alpha + beta");
    const auto* tokens = std::get_if<std::vector<Symbol>>(&lexed);
    return tokens && LRParser(grammar).accepts(*tokens) ? 0 : 1;
}
```

These are in-process compiler building blocks, not a sandbox for arbitrary
grammars or parser callbacks. Automata and table generation can grow large;
ambiguous grammars can produce large forests. A materialized `ParseNode` owns
its children recursively, so very deep trees may also need iterative teardown.
Apply appropriate input and resource limits before processing untrusted grammars.

## Rule-based diagnostics

Include `<liblr/DiagnosticsEngine.hpp>` to use `LRDiagEngine`. Register rules
in priority order with `addRule(pattern, action)`: the first matching pattern
chooses the action. If none matches, the engine returns a fatal diagnostic.
You can dispatch directly from a custom lexer or parser by passing an
`LRDiagState` to `diagnose`. The state carries the current source and
provenance, a half-open byte span, and, when available, the grammar, token
stream, token position, actual symbol, and expected symbols. Syntactic
diagnostics require the grammar and token stream. Views and pointers are
borrowed for the call; copy anything you need to keep.

Patterns can be callbacks or use the built-in adapters: `lexicalLiteral`
matches fixed bytes, `lexicalRegex` matches a byte regex, and `lexicalPEG`
and `syntacticPEG` match PEG prefixes. These adapters match at the error
position, not at an arbitrary later position in the input. Empty matches
count. `lexicalPEG` takes an `LRDiagByteMap` mapping bytes to the pattern
grammar's terminals; an unmapped byte anywhere in the suffix prevents a match.
`syntacticPEG` requires compatible terminal IDs and names. PEG `end()` tests
the actual end of the byte or token input.

An action can request an in-memory patch, report information or a warning,
or mark the failure as fatal. Messages and fix suggestions are data: the
library does not print, write the original source file, or terminate the
process. A byte patch replaces a range in the current source; a token patch
replaces a range in the current token stream. Empty ranges are insertions.
Fatal actions never edit input. Invalid states or actions and exceptions from
rule callbacks become fatal diagnostics. Register rules before dispatching;
callbacks must not modify the engine's rule list during dispatch.

For example, a lexer that rejects `@` can offer an in-memory replacement:

```cpp
using namespace firestone::lr;
LRDiagEngine diagnostics;
diagnostics.addRule(lexicalLiteral("@"), [](const LRDiagState& state) {
    return LRDiagAction{LRDiagSeverity::warning, "unexpected '@'",
        LRBytePatch{state.byteBegin, state.byteEnd, "+"}, "use '+' here"};
});
```

Use `LRPatchLexer(lexer, diagnostics).lex(source, provenance)` to recover from
lexical errors, or `LRPatchParser(lexer, diagnostics).parse(source, parser,
provenance)` with an `LLTableDriven` parser. A custom parser can supply its
grammar and a callback that returns an `LRParseAttempt` (`ParseNode` or
`LLSyntaxError`). Byte edits restart lexing; token edits retry parsing without
rewriting the source. Results retain the edited source, tokens, byte spans, and
diagnostics. EOF and synthetic tokens have zero-width spans; each diagnostic
records its coordinates before its edit.

An informational or warning action without a patch can skip an invalid byte, but
cannot advance a parser. No-op edits and exhausted patch budgets stop recovery.
`LRRecoveryOptions` sets the shared patch budget and can carry a cleanup
callback, called once on a fatal result. The caller decides whether and how to
report diagnostics or exit.

## Testing liblr

With Firestone tests built, run `ctest --test-dir build -R '^firestone\.lr_' --output-on-failure`.
The tests compare generalized top-down and bottom-up parsers with an independent
CFG recognizer on short inputs, including ambiguous grammars and nullable
cycles. On native builds with a C compiler, the tests also compile the emitted C99
scanner and check complete token streams against a reference matcher, including
all 256 single-byte inputs. The same corpus runs against emitted assembly on
supported Linux x86-64 systems.
