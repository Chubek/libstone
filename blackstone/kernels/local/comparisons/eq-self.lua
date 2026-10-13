lrewrite.kernel.define {
    name="eq_self", ir="ssa", phase="comparison_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x==x with Boolean true for the same SSA value or equal normalized integer literals. Signed and unsigned equality agree. Requires explicit modular built-in 1-64-bit integers for both operands; floating-point NaNs and named types are excluded. Keep operand evaluation, SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("eq_self")
