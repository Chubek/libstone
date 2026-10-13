lrewrite.kernel.define {
    name="fold_div", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold integer division only when constant operands lie in the common nonnegative signed/unsigned domain of a 2-64-bit type and the divisor is positive. The quotient truncates toward zero. Reject zero divisors, negative/high-bit operands, MIN/-1, unknown arithmetic semantics and noninteger types. Requires explicit modular semantics; preserves the CFG and result identity.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_div")
