lrewrite.kernel.define {
    name="copy_propagation", ir="ssa", phase="value_propagation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace operand uses of an integer SSA copy with its source operand, including uses on phi edges. Valid input SSA supplies dominance and type guarantees. Retain the now-unused copy definition because Firestone reserves stable ValueIds. Requires explicit modular built-in 1-64-bit integers; no memory, mutable-variable or textual substitution. Preserve CFG; refresh def-use and induction facts.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("copy_propagation")
