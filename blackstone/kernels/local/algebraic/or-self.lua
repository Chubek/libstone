lrewrite.kernel.define {
    name="or_self", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Apply bitwise idempotence x OR x -> x when operands have the same SSA identity or equal normalized literals. Requires explicit modular built-in 1-64-bit integers, including Boolean OR. Does not infer equality between unknown values. Keep operand evaluation, SSA identity and all CFG edges.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("or_self")
