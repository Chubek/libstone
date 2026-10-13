lrewrite.kernel.define {
    name="fold_eq", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold equality of same-width integer constants by comparing their modular bit patterns and emitting a Boolean constant. Signed and unsigned equality agree. Requires explicit modular built-in integers of width 1-64; never folds floating-point comparisons or named types. Preserve the SSA result and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_eq")
