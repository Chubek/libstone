lrewrite.kernel.define {
    name="fold_le", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold integer less-than-or-equal when known same-width operands have clear sign bits. This restricted domain has the same signed and unsigned ordering. Emit Boolean zero or one and retain SSA identity. Requires explicit modular 1-64-bit built-in integers; excludes high-bit inputs, floats and named types. CFG is preserved.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_le")
