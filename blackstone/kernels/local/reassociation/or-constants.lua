lrewrite.kernel.define {
    name="or_associate_constants", ir="ssa", phase="integer_reassociation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Combine the constants in nested bitwise OR into one width-bounded constant, accepting either operand order at each node. Requires explicit modular built-in 1-64-bit integers. This applies bitwise associativity without changing execution order or deleting inner definitions. Preserve SSA result identity and CFG; exclude named types and unknown arithmetic semantics.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("or_associate_constants")
