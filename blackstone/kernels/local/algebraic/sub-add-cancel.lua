lrewrite.kernel.define {
    name="sub_add_cancel", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Cancel (x-y)+y -> x, allowing the outer addition operands to be swapped. Match the repeated integer operand by SSA identity or normalized literal. Requires explicit modular built-in 1-64-bit semantics, including wraparound. No floating-point or trapping-arithmetic cancellation is authorized. Retain inner evaluation, result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("sub_add_cancel")
