lrewrite.kernel.define {
    name="fold_ge", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold greater-than-or-equal on known same-width integer constants with clear sign bits, where signed and unsigned relations agree. Emit Boolean zero or one. Requires explicit modular 1-64-bit built-in integers; excludes high-bit operands, floats, named types and unknown facts. Retain the SSA result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_ge")
