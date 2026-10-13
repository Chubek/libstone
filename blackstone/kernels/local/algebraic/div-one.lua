lrewrite.kernel.define {
    name="div_one", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x/1 with x for built-in 2-64-bit integers under explicit modular semantics. Division by positive one is exact for signed and unsigned values, including the minimum signed value. Never rewrite 1/x or division by minus one. Excludes floats and named types; preserves operand evaluation, SSA identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("div_one")
