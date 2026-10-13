lrewrite.kernel.define {
    name="and_all_ones", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace bitwise x AND an all-ones constant with x, in either order. Normalize the mask to the operand's built-in 1-64-bit width; Boolean true is the one-bit mask. Requires explicit modular semantics. Keep a stable SSA definition, operand evaluation and CFG; reject partial masks, named types and floats.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("and_all_ones")
