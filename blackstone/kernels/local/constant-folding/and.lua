lrewrite.kernel.define {
    name="fold_and", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Evaluate bitwise AND of two known, same-width built-in integer constants. Normalize operands and result to the explicit modular 1-64-bit domain; one-bit AND has Boolean semantics. Follow constant/copy definitions only. Exclude floats, named types and unknown semantics; retain the result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_and")
