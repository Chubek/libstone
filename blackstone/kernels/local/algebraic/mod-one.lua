lrewrite.kernel.define {
    name="mod_one", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace the remainder x%1 with a same-width zero for built-in 2-64-bit integers under explicit modular semantics. Signed and unsigned remainders agree for divisor positive one. Do not rewrite zero divisors or divisor minus one. Operand definitions remain evaluated; CFG and the SSA result identity are preserved.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("mod_one")
