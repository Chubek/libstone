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

Generated rules for groups and repetitions have ordinary grammar symbols
and can appear in parse trees. Avoid repetitions of nullable expressions
when a unique derivation is required.

## Byte lexers

`compile_regex` builds a Thompson NFA. Use `determinize` for a DFA that
supports `match_prefix`, and `minimize` to remove unreachable states and
combine equivalent states while preserving acceptance metadata. DFA
matching may return a zero-length match for a nullable regex.

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

`SIMDMatcher` and `SIMDLexer` use 128-bit lanes for runs that stay in the
same DFA state on supported GCC/Clang x86-64 and AArch64 targets. Other
transitions and unsupported targets use equivalent scalar matching. The
vector path can also be disabled explicitly when constructing either wrapper.

`emitC99` emits a standalone C99 translation unit for 8-bit-byte targets.
`emitAssembly` emits GNU-style x86-64 System V LP64 assembly for ELF targets,
with a limit of 65,535 DFA states. Only Linux x86-64 is exercised by the
integration test. Both emit a
scanner with the same C ABI:

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
scalar, for example `Lexer(utf8_scalar_dfa({1}))`. This does not change how the
byte regex engine interprets patterns.

## Token parsers

- `generateLRTable` builds canonical LR(1) tables by default, or LALR(1)
  tables with `LRMode::lalr1`. Inspect `conflicts()` to see every action in
  a conflicting cell. `LRParser` defaults to LALR(1), requires a
  conflict-free table, and returns a tree or `std::nullopt` on syntax
  failure. It does not use precedence to resolve conflicts.
- `GLRParser` defaults to canonical LR(1) and pursues all viable actions
  on a conflict. It returns a packed `ParseForest`; nodes share spans
  and their alternatives reference production IDs and child node IDs.
  Recursive derivations for the same span can form cycles, so traverse
  the forest with a visited set rather than treating it as a tree.
- `LLTable` computes nullable, FIRST, and FOLLOW sets and reports LL(1)
  conflicts and left recursion. `LLTableDriven` uses explicit stack
  frames; `LLFunctionDriven` dispatches through rule callables. Both
  require an LL(1) grammar and return a tree on success.
- `GLL` and `ATN` use generalized top-down call/return processing,
  including for left recursion and nullable cycles. Both offer
  `accepts` and `parse`; the latter returns one finite derivation, not
  the full forest. `ATN` exposes its network but has no semantic-action
  or register layer.
- `PEG` uses ordered choice by default and supports `PEGExpression`
  rule overrides for predicates, greedy repetition, and other operators.
  It rejects direct and indirect left recursion at construction, including
  through nullable prefixes, and reports successful zero-width repetition
  at parse time. `RecursiveDescent` and `ParserCombinator<T>` provide cursor
  and typed composition APIs for hand-written token parsers.

## Connecting lexing and parsing

The byte lexer and token parsers are independent. Assign each lexer rule
the numeric ID of its corresponding grammar terminal, or translate the
lexer’s IDs before parsing. Whitespace and comments can be skipped in the
lexer. Keep the lexer’s byte spans if you want to map parser diagnostics
back to source text.

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
#include <vector>

int main() {
    using namespace firestone::lr;
    const auto grammar = readEBNF("sum = 'word' { '+' 'word' };");
    const auto word = *grammar.findTerminal("word");
    const auto plus = *grammar.findTerminal("+");
    const auto lexer = make_lexer({{"[a-z]+", word}, {R"(\+)", plus}, {"[ \t]+", 0, true}});

    std::vector<Symbol> tokens;
    std::size_t cursor = 0;
    for (;;) {
        const auto token = lexer.next("alpha + beta", cursor);
        if (token.status == LexStatus::End) break;
        if (token.status == LexStatus::Error) return 1;
        tokens.push_back(token.token);
    }
    return LRParser(grammar).parse(tokens) ? 0 : 1;
}
```

These are in-process compiler building blocks, not a sandbox for arbitrary
grammars or parser callbacks. Automata and table generation can grow large;
ambiguous grammars can produce large forests. Apply appropriate input and
resource limits before processing untrusted grammars.
