lrewrite.kernel.define {
    name="fold_mul", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold two constant integer factors using modular 1-64-bit multiplication, including wraparound. Constant/copy chains are eligible. Preserve the SSA result and CFG; require explicit modular semantics and unnamed built-in integer types. Floating-point multiplication and overflow-sensitive arithmetic are excluded.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_mul")
