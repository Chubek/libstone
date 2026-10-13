lrewrite.kernel.define {
    name="shl_zero", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x shifted left by a proven zero count with x. Requires explicit modular built-in 1-64-bit integers; count zero is valid without assumptions about invalid-count handling. Do not rewrite shifts by other counts or overloaded operations. Keep SSA identity, operand evaluation and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("shl_zero")
