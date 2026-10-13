lrewrite.kernel.define {
    name="mul_zero", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer multiplication by a proven zero, on either side, with a same-width zero constant. Requires explicit modular built-in 1-64-bit integer semantics. Operand definitions are retained, so the kernel does not remove effects or traps from their evaluation. Floats and named types are excluded; CFG and SSA identity are preserved.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("mul_zero")
