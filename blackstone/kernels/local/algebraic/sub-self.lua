lrewrite.kernel.define {
    name="sub_self", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x-x by a same-width zero for identical SSA operands or equal normalized literals. Requires explicit modular built-in 1-64-bit integers; this rule is not a floating-point identity and cannot assume distinct values equal. Retain operand evaluation, the SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("sub_self")
