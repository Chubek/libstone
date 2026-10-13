lrewrite.kernel.define {
    name="fold_not", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold width-bounded bitwise complement of a known built-in integer constant. Mask to the explicit modular 1-64-bit width; the one-bit case agrees with Boolean NOT. Follow constant/copy definitions, excluding floats and named types. Preserve the SSA result identity and control-flow structure.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_not")
