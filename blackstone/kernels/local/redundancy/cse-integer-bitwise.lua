lrewrite.kernel.define {
    name="cse_integer_bitwise", ir="ssa", phase="redundancy_elimination",
    requires={"def_use", "dominators"}, preserves={"cfg", "dominators"},
    documentation=[[Replace repeated integer AND, OR, XOR or NOT by a copy of a matching dominating SSA result. Binary bitwise operands may be matched in either order; a same-block candidate must occur earlier. Requires explicit modular built-in 1-64-bit integers and valid dominance. No memory operations or effects are deduplicated. Retain result identities and CFG; refresh def-use facts.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("cse_integer_bitwise")
