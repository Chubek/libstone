lrewrite.kernel.define {
    name="cse_integer_arithmetic", ir="ssa", phase="redundancy_elimination",
    requires={"def_use", "dominators"}, preserves={"cfg", "dominators"},
    documentation=[[Replace a redundant modular integer add, subtract, multiply or negate by a copy of an equal expression whose definition dominates it. In one block the earlier expression must precede the use. Canonicalize operand order only for add and multiply. Requires built-in 1-64-bit integers; exclude division, shifts, memory, calls, floats and trapping arithmetic. Retain stable ValueIds and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("cse_integer_arithmetic")
