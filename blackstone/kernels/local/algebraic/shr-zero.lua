lrewrite.kernel.define {
    name="shr_zero", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace integer right shift by a proven zero count with its input. Arithmetic and logical right shifts agree at count zero, so signedness need not be guessed. Requires explicit modular built-in 1-64-bit integers. Preserve SSA identity, operand evaluation and CFG; no other count or invalid-shift policy is inferred.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("shr_zero")
