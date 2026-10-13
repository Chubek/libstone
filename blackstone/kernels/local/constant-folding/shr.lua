lrewrite.kernel.define {
    name="fold_shr", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold a constant right shift only when the input has a clear sign bit in its 2-64-bit type and the known count is in [0,width). Arithmetic and logical shifts then coincide. Reject negative/high-bit inputs, invalid counts and unknown semantics. Requires explicit modular integers; retains the SSA result and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_shr")
