# Gatebindle: language frontend bundles

Each subdirectory is a GateML bundle: a `MANIFEST` assembling `Profile.gml`
(identity), `Lexer.gml` (libchomsky3 patterns), `Parser.gml` (labeled BNF for
NovoParse/libglr), `AST.gml` (node kinds), `Adapter.gml` (Jiterati lowering
facilities), `Visitor.gml` (postorder handler metadata), and `Build.gml`
(facility metadata), plus a `README.md` with
scope notes.

| Bundle | Language covered | Notes |
|---|---|---|
| `C` | C integer expressions | Baseline seed: precedence layers, unary signs, C comments |
| `C++` | C++ integer expressions | Bitwise, shifts, `~`, assignment, calls, variables |
| `Java` | Java integer expressions | `>>>` unsigned shift alongside `<<` and `>>` |
| `Rust` | Rust integer expressions | `!` bitwise negation |
| `Python` | Python integer expressions | `#` comments, right-associative `**`, `//` |
| `Ruby` | Ruby integer expressions | `#` comments, right-associative `**` |
| `Delphi` | Delphi integer expressions | Case-insensitive word operators, `:=`, three comment forms |
| `Typescript` | TypeScript integer expressions | `$` identifiers, bitwise operators and unsigned `>>>` |
| `Scheme` | Scheme integer expressions | Signed literals, prefix arithmetic, `;` comments and named calls |

All bundles are executable expression seeds under a signed `i64` model, not
complete language frontends. Validate one with:

```sh
gateml-cli check gatestone/gatebindle/Python
gateml-cli lower gatestone/gatebindle/Delphi expression.pas
```
