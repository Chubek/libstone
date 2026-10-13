lrewrite.kernel.define {
    name="and_associate_constants", ir="ssa", phase="integer_reassociation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Combine two constant masks in nested bitwise AND: (x AND a) AND b -> x AND (a AND b). Accept either operand order, normalizing to the built-in 1-64-bit width. Requires explicit modular integers. The inner definition remains evaluated, so no side effects are removed. Preserve the SSA result identity and CFG; exclude named types.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("and_associate_constants")
