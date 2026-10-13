lrewrite.kernel.define {
    name="add_sub_cancel", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Cancel (x+y)-y -> x, allowing the inner addition operands to be swapped. The subtracted operand must have the same SSA identity or literal bit pattern as one addend. Requires explicit modular built-in 1-64-bit integers; intermediate overflow wraps. Retain inner definitions and evaluation; exclude floats and trapping arithmetic. Preserve result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("add_sub_cancel")
