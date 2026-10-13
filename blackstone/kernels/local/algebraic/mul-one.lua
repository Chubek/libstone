lrewrite.kernel.define {
    name="mul_one", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x*1 or 1*x with x in the explicit modular built-in 1-64-bit integer domain. Resolve one through constant/copy definitions only. Keep a stable SSA definition and all operand evaluation. No floating-point identities, overloaded operations or trapping arithmetic are assumed; CFG is preserved.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("mul_one")
