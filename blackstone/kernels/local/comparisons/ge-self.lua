lrewrite.kernel.define {
    name="ge_self", ir="ssa", phase="comparison_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x>=x by Boolean true for identical operands. Requires explicit modular built-in 1-64-bit integers; signedness does not change reflexivity, while floating-point NaNs are deliberately excluded. Distinct unknown values are not equated. Preserve operand evaluation, SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("ge_self")
