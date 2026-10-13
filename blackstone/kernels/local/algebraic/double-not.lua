lrewrite.kernel.define {
    name="double_not", ir="ssa", phase="algebraic_simplification",
    requires={"def_use"}, preserves={"cfg", "dominators"},
    documentation=[[Replace width-bounded complement of a complement by its original input. Requires a matching inner SSA NOT definition and explicit modular built-in 1-64-bit integers; one-bit Boolean double negation is included. Inner definitions remain evaluated. Exclude named types and floats; preserve result identity and CFG.]]
}
lrewrite.kernel.run = lrewrite.local_rewrite("double_not")
