# Scheme expression seed

Parses signed decimal integers, ASCII identifiers, semicolon line comments,
binary prefix `+`, `-`, `*`, `quotient`, `remainder`, unary `-`, and named
zero/one-argument calls. For example: `(+ 1 (* 2 3))` and `(- -12)`.

This is a signed **i64** subset, not a complete Scheme implementation. It has
no arbitrary-precision numbers, ratios, strings, lists, quotation, lambdas,
definitions, booleans or variadic arithmetic. Only the documented arities
are accepted; `(1)` is not grouping. `quotient` truncates toward zero and
`remainder` uses signed remainder semantics.

`variable`, `namedcall` and `namedcallempty` are host facilities. Named-call
hooks receive the original `IDENT` terminal in the tree, plus zero/one lowered
argument values (there is no lowered receiver). `Visitor.gml` supplies
postorder node-to-handler metadata.

```sh
gateml-cli check gatestone/gatebindle/Scheme
gateml-cli lower gatestone/gatebindle/Scheme expression.scm
```
