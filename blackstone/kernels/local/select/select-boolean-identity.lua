lrewrite.kernel.define {
    name="select_boolean_identity", ir="ssa", phase="select_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace select(c,true,false) with the Boolean condition c. Require a one-bit integer result, proven literal/constant/copy arms and explicit modular built-in integer semantics. Do not apply to wider integers or merely truthy values. Arm and condition definitions remain evaluated; retain SSA identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("select_boolean_identity")
