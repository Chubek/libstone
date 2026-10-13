lrewrite.kernel.define {
    name="subtract_negation", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Rewrite x-(-y) as x+y using the inner SSA negation definition. Requires explicit modular built-in 1-64-bit integer semantics, so intermediate wraparound is legal. The inner negation remains defined and evaluated. This is not floating-point reassociation and excludes trapping arithmetic and named types; preserve SSA identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("subtract_negation")
