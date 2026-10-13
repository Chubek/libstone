lrewrite.kernel.define {
    name="xor_cancel", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Cancel a repeated identical operand in (x XOR y) XOR y -> x, accepting commuted operand orders at both XORs. Requires explicit modular built-in 1-64-bit integers and an inner SSA XOR definition. Do not assume unknown values equal or erase their definitions. Retain the SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("xor_cancel")
