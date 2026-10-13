lrewrite.kernel.define {
    name="and_absorption", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Apply x AND (x OR y) -> x for matching integer SSA operands, accepting commuted positions at both nodes. Requires explicit modular built-in 1-64-bit integers. Do not equate distinct unknown values; the inner OR and operand-producing definitions remain evaluated. Keep the result as a copy or constant, preserving SSA identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("and_absorption")
