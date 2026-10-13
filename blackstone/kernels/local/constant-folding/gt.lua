lrewrite.kernel.define {
    name="fold_gt", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold greater-than on known same-width integer constants only in the clear-sign-bit domain, avoiding assumptions about signedness. Emit a Boolean constant. Requires explicit modular 1-64-bit built-in integers and constant/copy facts. Floats, named types, high-bit inputs and unknown semantics are excluded; CFG and SSA identity are preserved.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_gt")
