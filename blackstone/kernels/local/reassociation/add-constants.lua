lrewrite.kernel.define {
    name="add_associate_constants", ir="ssa", phase="integer_reassociation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Combine the constants in (x+a)+b into x+(a+b), allowing either operand order at each integer addition. Compute the combined constant modulo the declared 1-64-bit width. Requires explicit modular built-in integers, so intermediate wraparound is permitted. Keep the inner definition and its evaluation. Floats, named types and trapping arithmetic are excluded; preserve result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("add_associate_constants")
