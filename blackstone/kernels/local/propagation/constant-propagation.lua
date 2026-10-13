lrewrite.kernel.define {
    name="constant_propagation", ir="ssa", phase="value_propagation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Substitute integer literal operands for uses of a constant SSA definition, without evaluating arithmetic. Retain the unused constant definition to preserve stable ValueIds. Requires valid SSA, explicit modular built-in 1-64-bit integers and a literal constant, not an unknown analysis fact. No branch pruning or folding is included; CFG is preserved and value-dependent analyses are invalidated.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("constant_propagation")
