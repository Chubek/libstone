lrewrite.kernel.define {
    name="select_true", ir="ssa", phase="select_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace select(true,x,y) with x when a literal or constant/copy definition proves the Boolean condition true. Require explicit modular built-in 1-64-bit integer arms of matching type. This changes a value operation, not a branch; both arm-producing definitions remain evaluated. Preserve SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("select_true")
