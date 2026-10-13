lrewrite.kernel.define {
    name="fold_or", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold bitwise OR of known same-width integers in the explicit modular 1-64-bit domain. One-bit OR agrees with Boolean OR. Constant/copy definitions may supply values; other definitions remain unknown. Exclude noninteger and named types. Preserve the SSA result and CFG, and refresh value-dependent analyses.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_or")
