lrewrite.kernel.define {
    name="double_negation", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace -(-x) by x when the inner SSA definition is an integer negation. Requires explicit modular built-in 1-64-bit integers, including wraparound at the minimum signed bit pattern. Keep the inner definition and all operand evaluation. Trapping arithmetic, floats and named types are excluded; result identity and CFG are retained.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("double_negation")
