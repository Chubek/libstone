lrewrite.kernel.define {
    name="fold_neg", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Evaluate negation of a known built-in integer constant modulo its declared 1-64-bit width. The minimum signed bit pattern wraps instead of invoking host overflow. Requires explicit modular semantics; excludes floating-point, named types and trapping negation. Retain the SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_neg")
