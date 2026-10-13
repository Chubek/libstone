lrewrite.kernel.define {
    name="gt_self", ir="ssa", phase="comparison_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer x>x by Boolean false for equal operand identities or equal normalized literals. Signed and unsigned greater-than are both irreflexive. Requires explicit modular built-in 1-64-bit integer operands. No floats, named types or unknown equalities are accepted; retain operand evaluation, result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("gt_self")
