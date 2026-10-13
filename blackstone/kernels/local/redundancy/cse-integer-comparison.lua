lrewrite.kernel.define {
    name="cse_integer_comparison", ir="ssa", phase="redundancy_elimination",
    requires={"def_use", "dominators"}, preserves={"cfg", "dominators"},
    documentation=[[Replace a repeated integer comparison by a copy of the same operation on the same operands from a dominating definition. Only equality and inequality allow swapped operand order; ordered relations retain their order. Signedness need not be guessed because the operation is unchanged. Requires explicit modular built-in 1-64-bit operands; floats are excluded. Preserve stable result identities and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("cse_integer_comparison")
