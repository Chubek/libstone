lrewrite.kernel.define {
    name="xor_self", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x XOR x by a same-width zero when operands are the same SSA value or equal normalized integer literals. Requires explicit modular built-in 1-64-bit integer semantics. Does not equate distinct unknown values or eliminate operand definitions. Preserve the result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("xor_self")
