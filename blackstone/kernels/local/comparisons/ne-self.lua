lrewrite.kernel.define {
    name="ne_self", ir="ssa", phase="comparison_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x!=x with Boolean false when operand identities are equal. Requires explicit modular built-in 1-64-bit integer operands, so floating-point NaNs cannot invalidate the rule. Do not infer equality of distinct unknown values. Retain operand evaluation, SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("ne_self")
