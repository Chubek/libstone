lrewrite.kernel.define {
    name="cse_select", ir="ssa", phase="redundancy_elimination",
    requires={"def_use", "dominators"}, preserves={"cfg", "dominators"},
    documentation=[[Replace a repeated integer select with a copy of an equal select that dominates it and has the same Boolean condition and ordered arms. Same-block candidates must precede the duplicate. Requires explicit modular built-in 1-64-bit arms and valid SSA; no branch, memory or floating-point transformation is implied. Keep operand evaluation, stable ValueIds and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("cse_select")
