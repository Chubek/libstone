lrewrite.kernel.define {
    name="fold_add", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold integer addition with two known constants in the explicitly selected modular 1-64-bit domain. Resolve constant/copy definitions, mask the result to its declared width, and retain its SSA identity. No floating-point, named-type, trapping-arithmetic or unknown-semantics evaluation. Def-use and induction facts must be refreshed.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_add")
