# GateML v0.1

## Object model

A frontend contains an ordered lexical rule list, a start nonterminal, labeled
BNF productions, declared AST node kinds, node-to-facility lowering bindings,
and string-valued profile metadata. Definition order determines lexical
priority and descriptor ordering. Section order is unrestricted; sections
may repeat. Exactly one start declaration is required.

Identifiers are ASCII `[A-Za-z_][A-Za-z0-9_]*`. Keywords from the EBNF and
NovoParse's `__np_` prefix are reserved. Strings support `\"`, `\\`, `\n`,
`\r`, and `\t`. Regex backslashes therefore require escaping in GateML
strings. Raw newlines and NUL are invalid. Definition comments use `//`.

## Lexing

`token NAME = "ERE";` produces a named terminal. `skip NAME = "ERE";`
consumes trivia. Both participate in longest-prefix matching; equal-length
matches prefer the earliest declaration. Patterns compile through
libchomsky3. Lexical matching is anchored at the current source offset;
nullable patterns are rejected. Unmatched bytes are errors.

libchomsky3's bytecode VM selects the first successful regex alternative.
For patterns containing alternation, GateML additionally checks complete
prefixes in descending length order to recover the longest lexical match.
This correctness-first fallback can be quadratic in source length for
alternation-heavy patterns. Separate lexical rules are preferable for simple
alternatives. The pattern language is libchomsky3's ERE dialect; its default
flags are used. No case-folding or multiline mode is implicitly enabled.

Non-skipped terminals receive deterministic one-byte codes in declaration
order from `A-Z`, `a-z`, `0-9`. Version 0.1 supports at most 62 terminals.
Codes are an internal parser transport, not source-language characters.

## Parsing and AST

`rule HEAD = SYMBOL ... => NODE | ...;` declares one or more productions.
Symbols reference terminals or nonterminals. An alternative with no symbols
is epsilon. `start HEAD;` selects the entry nonterminal. `node NODE;`
declares the production's AST kind. Rules may be declared separately for
the same head; exported NovoParse grammar groups them into one rule.

Terminals and nonterminals share a symbol namespace. Skip rules cannot appear
in productions. Undefined symbols, duplicate productions with identical
head/body, undefined AST kinds, duplicate declarations, and nonproductive
nonterminals are errors. Unused lexical rules and AST kinds are permitted.
Every reachable production must derive finite input through productive heads.

AST trees preserve all grammar children, including punctuation terminals and
identity nodes. Nonterminals are named by production labels, while terminals
retain lexical rule names and original text. There is no implicit AST rewrite.
Ambiguous accepted input is rejected by NovoParse/libglr. Encode precedence
through grammar layers; the C bundle illustrates left-associative binary
operators and right-recursive unary operators.

Trees own their data independently of parser lifetime. Spans use half-open
original byte offsets and one-based line/byte-column positions. A parent span
includes trivia between its children, but excludes leading/trailing trivia.
An empty node is located at the next token or EOF. EOF diagnostics include
trailing trivia. CR increments byte column; LF advances line and resets column.

## Semantic adapters

`lower NODE = FACILITY;` binds an AST kind to a named semantic operation.
Bindings are optional for parsing. Lowering requires implementations for every
visited nonterminal. Built-ins produce signed `i64` Jiterati values:

| Facility | Input | Jiterati operation |
|---|---|---|
| `integer` | One terminal containing a decimal i64 | `const_i64` |
| `identity` | One nonterminal | Forward value |
| `neg`, `not` | One nonterminal | `Block::neg`, `Block::bitwise_not` |
| `add`, `sub`, `mul` | Two nonterminals | Corresponding arithmetic builder |
| `sdiv`, `srem` | Two nonterminals | Signed division/remainder builder |
| `band`, `bor`, `bxor` | Two nonterminals | `Block::bitwise_and/or/xor` |
| `shl`, `lshr`, `ashr` | Two nonterminals | `Block::shl/lshr/ashr` |

Built-in arities are checked against every production bearing the bound AST
kind. Punctuation terminals are omitted from the value argument list but remain
accessible in the source tree. Integer conversion checks the complete lexeme
and rejects overflow. Runtime division behavior belongs to the selected
Jiterati backend; GateML emits semantic IR rather than evaluating expressions.

Embedding applications supply `LoweringHooks`, a map from facility name to
callback. A callback receives the tree node, current function/block, and
already-lowered nonterminal values. It must return an i64 value owned by the
current function. Custom hooks take precedence over built-ins. Unknown
facility names are preserved during compilation for later backend resolution.
The reference bundles use this mechanism for `assign`, `variable`, `call`,
`callempty`, and `pow`: those nodes parse without implementations, while
lowering them without host hooks is a located error.
The built-in lowering entry point produces one zero-argument `entry` function.

## Profiles and bundles

`option KEY = "VALUE";` declares opaque metadata. Duplicate keys are errors.
Options are preserved for later facility backends; they do not alter lexer,
parser, or lowering algorithms. The current semantic value type is always
i64, regardless of metadata.

The D reference visitor reads `visitor_order` (`preorder` or `postorder`),
`visitor_terminals` (`true` or `false`), and `visit_NODE` handler-name options.
The reference bundles declare postorder, nonterminal-only handlers matching
their lowering facilities. These remain opaque metadata to the C++ bootstrap.

A bundle directory contains `MANIFEST`, with one local `.gml` filename per
line. Blank lines and lines beginning with `#` are ignored. Filenames cannot
contain parent directories or be absolute, cannot contain NUL, and cannot repeat.
Both LF and CRLF line endings are accepted. Files are concatenated in manifest
order with newline separators. The first fragment provides the frontend header.
Located failures are mapped back to their original fragment, including the
fragment-relative line and byte column. A `MANIFEST` that cannot be read, a
missing fragment, and an invalid entry are located failures rather than raw
filesystem exceptions.

## Descriptor format

`gateml-cli compile` emits JSON with `format: "gateml.frontend"`, `version: 1`,
`name`, `start`, `value_type: "i64"`, `lexer`, `productions`, `nodes`,
`lowering`, and `options`. Each lexical entry has `name`, `pattern`, `skip`,
and `code` (null for trivia). Productions contain `head`, ordered `body`, and
`node`. Lowering/options are string maps sorted by key; arrays retain source
order. JSON excludes source locations for reproducibility across bundle paths.

The descriptor is a frontend contract for future generators and facility
backends. The C++ bootstrap exposes serialization but no descriptor loader;
the D reference library provides `serde.JSON.readDescriptor` with validation.
`grammar` exports validated NovoParse BNF over token codes, suitable
for NovoParse's C, C++, D, or Python generator profiles. Generated parsers
expect the token tape, with the lexical mapping in the frontend descriptor.
