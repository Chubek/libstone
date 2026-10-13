lrewrite.kernel.define {
    name="mul_associate_constants", ir="ssa", phase="integer_reassociation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Combine constant factors in (x*a)*b into x*(a*b), allowing commuted factor positions. Multiply the constants modulo the built-in 1-64-bit integer width. Requires explicit modular semantics; do not reassociate floats or trapping arithmetic. Retain the inner SSA definition and all operand evaluation, keeping result identity and CFG. Profitability remains target-dependent.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("mul_associate_constants")
