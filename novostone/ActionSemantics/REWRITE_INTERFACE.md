# Rewrite frontend integration (in progress)

The rewrite implementation adds these ASDL nodes:

```
rewrite_set = (string name, arm* arms, usize offset)
bank_ref = (string name, usize offset)
action += Rewrite(string strategy, expr subject, bank_ref* banks, arm* arms)
document += rewrite_set* rewrites  # between data_sorts and rules
```

`arm` remains `MatchArm(pattern head, expr? guard, action body)`.
Guards are ordinary pure yielders; captures use `bound("capture")`.
Replacement bodies are actions and must complete with one datum and no bindings.
The loader can resolve external banks to `arm` nodes before lowering, or expand
external banks into local `rewrite NAME = { ... };` declarations in parsed input.
References are ordered, and bank arm order is preserved.

Initial grammar:

```
rewrite Simplify = { Pair(x, 0) => give bound("x") };
rule Run = rewrite saturate given(1) from bank(Simplify);
rule Other = saturate given(1) using { Pair(x, 0) => give bound("x") };
```

The rewrite agent owns ASDL/grammar, lowering, Model/Generate, four runtimes,
and rewrite tests. Bank integration should coordinate precise patches in these
files. Bank reference names are identifiers or quoted strings (the bank agent
may widen the quoted bank path token independently of ordinary token literals).
Lua guards need a distinct expression node/opcode or compile-time evaluated
expression; do not overload numeric guard IDs or bypass the grammar.
