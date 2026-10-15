# Delphi frontend reference bundle

Expression seed for Delphi with `//`, `{ }`, and `(* *)` comments
(non-nesting in this seed). Word operators are case-insensitive through
explicit character-class patterns: `div` (integer division), `mod`, `shl`,
`shr` (logical shift, lowering to `lshr`), `and`, `or`, `xor`, and `not`.
Assignment uses `:=`. Keyword rules precede `IDENT` so equal-length matches
resolve to keywords while longer identifiers (such as `divide`) still lex
as `IDENT` through longest-prefix matching.

`/` (real division) lowers to truncating `sdiv` under the signed `i64`
model. Units, types, sets, strings, and statements are future language
extensions. `Assign`, `Variable`, `Call`, and `CallEmpty` bind facility
names for later backends.
