lrewrite.kernel.define {
    name="lt_self", ir="ssa", phase="comparison_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x<x by Boolean false for identical operands. Signed and unsigned ordering both satisfy irreflexivity; no signedness guess is needed. Requires explicit modular built-in 1-64-bit integer operands; floats and named types are excluded. Keep operand definitions, result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("lt_self")
