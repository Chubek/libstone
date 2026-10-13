lrewrite.kernel.define {
    name="fold_sub", ir="ssa", phase="constant_evaluation",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Fold subtraction of two known integer constants modulo the declared 1-64-bit width. Constant and copy definitions may supply operands. Retain the result identity and CFG. Requires explicit modular semantics; does not evaluate floats, named integer types or trapping arithmetic.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("fold_sub")
