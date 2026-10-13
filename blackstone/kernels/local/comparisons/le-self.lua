lrewrite.kernel.define {
    name="le_self", ir="ssa", phase="comparison_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x<=x by Boolean true when operands have equal SSA identity or equal normalized literals. Requires explicit modular built-in 1-64-bit integer operands; excludes floats where NaNs would invalidate reflexivity. Preserve operand evaluation, SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("le_self")
