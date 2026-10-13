lrewrite.kernel.define {
    name="or_complement", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x OR NOT(x) with an all-ones constant normalized to its width, accepting either outer operand order. Requires a matching inner SSA NOT and explicit modular built-in 1-64-bit integers; one-bit output is true. Keep inner definitions and operand evaluation, so no effects are removed. Preserve result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("or_complement")
