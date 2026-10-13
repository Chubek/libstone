lrewrite.kernel.define {
    name="add_negation", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x+(-x) or (-x)+x by a same-width zero using an inner SSA negation definition. The operands must share identity, not merely similar syntax. Requires explicit modular built-in 1-64-bit integers; overflow-sensitive and floating-point arithmetic are excluded. Retain inner evaluation, result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("add_negation")
