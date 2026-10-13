lrewrite.kernel.define {
    name="and_complement", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x AND NOT(x) with a same-width zero, accepting either outer operand order. Match the complemented operand through an SSA NOT definition and exact integer operand identity. Requires explicit modular built-in 1-64-bit semantics, including Boolean conjunction. Inner and operand definitions remain evaluated; retain result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("and_complement")
