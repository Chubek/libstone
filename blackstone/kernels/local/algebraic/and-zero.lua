lrewrite.kernel.define {
    name="and_zero", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x AND zero, in either operand order, with a same-width zero constant. Requires explicit modular built-in 1-64-bit integers and a proven zero. The one-bit case is Boolean conjunction. Keep operand definitions, result identity and CFG; do not apply overloaded or floating-point identities.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("and_zero")
