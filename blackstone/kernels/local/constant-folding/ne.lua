lrewrite.kernel.define {
    name="fold_ne", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold inequality of known same-width integer constants using normalized modular bit patterns. Emit Boolean zero or one while retaining the result identity. Signedness does not affect equality. Requires explicit modular 1-64-bit built-in integers; excludes floats, named types and unknown facts. Preserve CFG and dominators.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_ne")
