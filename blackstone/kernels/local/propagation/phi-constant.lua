lrewrite.kernel.define {
    name="phi_constant", ir="ssa", phase="ssa_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace an integer phi whose every incoming edge carries the same literal bit pattern with a constant definition. Move the replacement after remaining phis to preserve Firestone ordering and retain its ValueId. Requires explicit modular built-in 1-64-bit integers. Do not infer constants from loop-dependent definitions or change predecessor edges. Def-use and induction facts must be refreshed.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("phi_constant")
