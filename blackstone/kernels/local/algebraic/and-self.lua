lrewrite.kernel.define {
    name="and_self", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Apply bitwise idempotence x AND x -> x for identical SSA operands or equal normalized literals. Requires explicit modular built-in 1-64-bit integers, including Boolean conjunction. Keep a stable result definition and operand evaluation. Distinct unknown values and named types are excluded; CFG is preserved.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("and_self")
