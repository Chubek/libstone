lrewrite.kernel.define {
    name="or_all_ones", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace bitwise OR with a proven all-ones mask, in either operand order, by that mask normalized to the result width. Requires explicit modular built-in 1-64-bit integers; one-bit output is true. Keep operand definitions so effects are not erased. Preserve SSA identity and CFG; exclude partial masks, floats and named types.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("or_all_ones")
