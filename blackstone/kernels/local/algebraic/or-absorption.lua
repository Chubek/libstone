lrewrite.kernel.define {
    name="or_absorption", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Apply x OR (x AND y) -> x for identical integer SSA operands, accepting either position at each node. Requires explicit modular built-in 1-64-bit integer semantics. Inner definitions and their evaluation remain intact; no memory or effectful instruction is deleted. Distinct unknown values do not match. Preserve result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("or_absorption")
