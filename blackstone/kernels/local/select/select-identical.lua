lrewrite.kernel.define {
    name="select_identical", ir="ssa", phase="select_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace select(c,x,x) with x when the integer arms share SSA identity or equal normalized literals. Requires explicit modular built-in 1-64-bit integer arms and a valid Boolean condition. The condition-producing definition remains evaluated, as do the arms. Unknown arm equality is not assumed; preserve result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("select_identical")
