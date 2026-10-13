lrewrite.kernel.define {
    name="fold_shl", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold a constant integer left shift in the modular 2-64-bit domain, discarding shifted-out bits. Require a known shift count in [0,width); reject invalid counts rather than inventing trap or count-masking semantics. Exclude floats, named types and unknown arithmetic semantics. Preserve the SSA identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_shl")
