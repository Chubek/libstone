lrewrite.kernel.define {
    name="xor_associate_constants", ir="ssa", phase="integer_reassociation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Combine constant operands in nested XOR: (x XOR a) XOR b -> x XOR (a XOR b), allowing commuted positions. Normalize the combined literal to the built-in 1-64-bit width. Requires explicit modular semantics. Retain inner definitions and operand evaluation; do not infer identities for named or noninteger types. Preserve result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("xor_associate_constants")
