lrewrite.kernel.define {
    name="sub_sub_cancel", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Rewrite x-(x-y) -> y when the outer minuend matches the inner minuend by integer SSA identity or normalized literal. Requires explicit modular built-in 1-64-bit integers; intermediate wraparound is permitted. Keep inner definitions and operand evaluation. Exclude floats, named types and trapping arithmetic; preserve result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("sub_sub_cancel")
