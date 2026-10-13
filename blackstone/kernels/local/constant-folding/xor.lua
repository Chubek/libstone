lrewrite.kernel.define {
    name="fold_xor", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold the bitwise exclusive OR of two known same-width integer constants. Normalize the result modulo its declared 1-64-bit width under explicit modular semantics; the one-bit case is Boolean exclusive OR. Exclude floating-point and named types. Retain the SSA identity and control-flow structure.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_xor")
