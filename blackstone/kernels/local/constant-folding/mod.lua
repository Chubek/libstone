lrewrite.kernel.define {
    name="fold_mod", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Evaluate the remainder of nonnegative integer constants with a positive divisor, within the common signed/unsigned domain of a 2-64-bit type. Exclude negative/high-bit constants and zero divisors so signedness and exception behavior cannot change. Requires explicit modular semantics. Retain the SSA identity and all control-flow edges.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_mod")
