lrewrite.kernel.define {
    name="xor_zero", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace x XOR zero, in either order, with x for explicit modular built-in 1-64-bit integers. Constant/copy facts may prove the zero operand. Keep the SSA result as a copy or constant and retain all operand evaluation. Boolean exclusive OR is included; floats, named types and unknown semantics are excluded. CFG is preserved.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("xor_zero")
