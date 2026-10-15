# TypeScript expression seed

Parses decimal integers, ASCII identifiers (including `$`), parentheses,
unary `+`, `-`, `~`, arithmetic, bitwise operators, shifts (`<<`, `>>`, `>>>`),
right-associative assignment, and zero/one-argument calls. Supports `//` and
`/* */` comments. Precedence is encoded in the grammar.

This is not a full TypeScript frontend: types, declarations, strings, floating
point, exponentiation, statements and multiple arguments are not implemented.
Like the other Gatebindle seeds it uses signed **i64**, not JavaScript `Number`
or JavaScript's 32-bit bitwise coercions. `/` lowers to signed integer division;
`>>>` lowers to a 64-bit logical shift. These are intentional approximations.

`assign`, `variable`, `call`, and `callempty` require host facilities when
executing/lowering. `Visitor.gml` provides postorder node-to-handler metadata.

```sh
gateml-cli check gatestone/gatebindle/Typescript
gateml-cli parse gatestone/gatebindle/Typescript expression.ts
```
