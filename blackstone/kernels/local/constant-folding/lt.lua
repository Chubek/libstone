lrewrite.kernel.define {
    name="fold_lt", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold integer less-than only for known same-width constants with clear sign bits, where signed and unsigned ordering coincide. Emit a Boolean constant. High-bit operands, floats, named types and unknown arithmetic semantics remain untouched. Requires explicit modular 1-64-bit integers; retains SSA identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_lt")
