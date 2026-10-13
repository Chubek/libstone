lrewrite.kernel.define {
    name="select_boolean_inversion", ir="ssa", phase="select_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Rewrite select(c,false,true) as Boolean c XOR true. Require a one-bit result, proven Boolean arms and explicit modular built-in integer semantics. Wider integer selects are not Boolean negations. Keep arm and condition definitions evaluated, retain the SSA result identity, and preserve CFG and dominators.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("select_boolean_inversion")
