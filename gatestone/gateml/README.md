# GateML

GateML specifies reusable language frontends: lexical rules, labeled grammars,
AST node kinds, semantic lowering hooks, and profile metadata. A definition
compiles to a backend-independent descriptor; source programs parse to owned
trees and can lower to Jiterati IR.

## Build

Requirements: CMake 3.20+, C/C++17 compilers, a D compiler (`ldc2`, `dmd`, or
`gdc`), and Python 3 for tests. Dependencies are taken from this checkout;
the build performs no downloads.

```sh
cmake -S gatestone -B build/gatestone -DBUILD_TESTING=ON
cmake --build build/gatestone --parallel
ctest --test-dir build/gatestone --output-on-failure
build/gatestone/gateml/gateml-cli check gatestone/gatebindle/C
```

`gatestone/gateml` also builds standalone. The root Libstone build exposes
`-DLIBSTONE_BUILD_GATESTONE=ON`; it defaults to OFF.

## Definition

```gml
frontend Arithmetic;
lexer {
    skip WS = "[ \t\r\n]+";
    token NUMBER = "[0-9]+";
    token PLUS = "\\+";
}
parser {
    start expression;
    rule expression = expression PLUS atom => Add | atom => Pass;
    rule atom = NUMBER => Integer;
}
ast { node Add; node Pass; node Integer; }
adapter {
    lower Add = add;
    lower Pass = identity;
    lower Integer = integer;
}
profile { option language = "Arithmetic"; }
```

Definitions use one `frontend Name;` header followed by repeatable sections.
Every production declares its AST kind. Precedence and associativity are
expressed structurally through nonterminal layers and recursion.

## Commands

```sh
gateml-cli check path/to/frontend.gml
gateml-cli compile gatestone/gatebindle/C > C.frontend.json
gateml-cli grammar gatestone/gatebindle/C > C.ng
gateml-cli parse gatestone/gatebindle/C expression.c
gateml-cli lower gatestone/gatebindle/C expression.c
```

`compile` emits a deterministic, versioned frontend descriptor. `grammar`
emits NovoParse grammar over token codes. `parse` emits source-tree JSON;
`lower` emits a zero-argument `i64` Jiterati function. Commands accept a
single definition file or a bundle directory, and support `--help`.
Exit codes are 0 for success, 1 for input/compilation failure, and 2 for usage.

## Implementation

```text
GateML text ──libchomsky3──> token tape ──NovoParse/libglr──> Frontend
source text ──libchomsky3──> token tape ──NovoParse/libglr──> owned Tree
Tree + declared adapter hooks ──Jiterati builders──> semantic IR
Frontend ──versioned JSON──> future facility backends
```

`GateML.g` is compiled by the vendored NovoParse D frontend during the build.
Its generated C++ parser and the source-language parsers use NovoParse's native
runtime linked against **`third_party/libglr`**, rather than NovoParse's nested
copy. libchomsky3 owns regex compilation and matching for both GateML and the
defined languages. NovoParse sees only a one-byte token tape; it does not
retokenize original source with its internal regex scanner. The tape-to-source
map restores terminal lexemes, byte spans, and diagnostic locations.

GateML links Jiterati's header-only core directly. Lowering produces real
`jiterati::Module`, `Function`, `Block`, and `Value` objects, which can be handed
to Jiterati's backend APIs. The returned module has stable heap ownership.
Facility backends can supply named `LoweringHooks` callbacks and emit their own
Jiterati operations. Unknown hook names remain in frontend descriptors and
produce a located error if lowering is attempted without an implementation.

The C reference bundle supports decimal integer expressions, unary signs,
parentheses, `+`, `-`, `*`, `/`, `%`, and C comments. The remaining bundles
extend the same expression-seed pattern with language-specific lexing:

| Bundle | Frontend | Distinctions |
|---|---|---|
| `gatebindle/C` | `C` | C comments; additive/multiplicative/unary layers |
| `gatebindle/C++` | `Cpp` | `//`, `/* */`; bitwise, shifts, `~`, assignment, calls, variables |
| `gatebindle/Java` | `Java` | Adds unsigned `>>>` (`lshr`); `>>` lowers to `ashr` |
| `gatebindle/Rust` | `Rust` | `!` for bitwise negation; `//`, `/* */` |
| `gatebindle/Python` | `Python` | `#` comments; right-associative `**` (`pow` facility); `//` |
| `gatebindle/Ruby` | `Ruby` | `#` comments; right-associative `**` (`pow` facility) |
| `gatebindle/Delphi` | `Delphi` | `//`, `{ }`, `(* *)`; case-insensitive `div`, `mod`, `shl`, `shr`, `and`, `or`, `xor`, `not`; `:=` |

Each bundle beyond C binds `assign`, `variable`, `call`, and `callempty`
facilities for later backends; Python and Ruby additionally bind `pow`.
These parse without host hooks and lower once an embedding application
supplies them. All seeds share the signed `i64` semantic model with
documented per-language approximations (see each bundle's `README.md`).
Full language coverage — declarations, types, statements, and standard
libraries — remains future work.

The earlier empty `.d` files remain design scaffolds. The executable bootstrap
compiler lives in `src/`, using C++ for direct Jiterati integration and D for
NovoParse grammar generation.

See [the language specification](specs/GateML.md),
[the EBNF](specs/GateML.ebnf), and
[the dependency integration notes](docs/dependencies.md).
