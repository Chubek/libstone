lrewrite.kernel.define {
    name="select_false", ir="ssa", phase="select_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace select(false,x,y) with y when the Boolean condition is proven false by a literal or constant/copy definition. Requires explicit modular built-in 1-64-bit integer arms. Does not alter branches or remove arm-producing effects. Retain the SSA result identity and CFG; unknown conditions stay unchanged.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("select_false")
