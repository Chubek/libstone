lrewrite.kernel.define {
    name="sub_zero", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x-0 with x, retaining the SSA result as a copy or constant. Requires a proven zero right operand and explicit modular built-in 1-64-bit integers. Does not rewrite 0-x, floating-point subtraction or named types. Operand-producing instructions remain evaluated; CFG is preserved.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("sub_zero")
