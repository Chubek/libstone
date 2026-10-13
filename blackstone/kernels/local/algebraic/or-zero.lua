lrewrite.kernel.define {
    name="or_zero", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x OR zero, in either order, with x in the explicit modular built-in 1-64-bit domain. Zero must be proven by a literal or constant/copy definition. The Boolean case follows the same identity. Preserve operand evaluation, SSA identity and CFG; exclude floating-point and named types.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("or_zero")
