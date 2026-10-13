lrewrite.kernel.define {
    name="add_zero", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x+0 or 0+x with x for built-in integers of width 1-64 under explicit modular semantics. Zero may come from a constant/copy definition. Keep the result as an SSA copy or constant, preserving operand evaluation and CFG. No floating-point or overloaded identities are authorized.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("add_zero")
