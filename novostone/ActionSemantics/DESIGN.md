# Synthesis pipeline

```text
.acsem
  -> generated Aurocks C parser
  -> ASDL arena-owned document/sort/constructor/rule/action/expr tree
  -> name/sort validation + generated ASDL postorder visitor
  -> immutable typed declaration tables and checked instruction indices
  -> C11 / Python / Rust / Pascal tables + embedded target action machine
  -> standalone constructor-term interpreter
  -> or LaTeX syntax and semantic equations
```

`ActionSemantics.g` owns the specification grammar. Every node that can produce
a semantic diagnostic carries its source offset. Persistent parse lists and
abandoned parser alternatives share one ASDL arena. The internal C syntax
wrapper owns that arena and frees it on either outcome. The C++ entry point
serializes parser state, then elaborates outside the lock. Generated external
Aurocks helpers are prefixed `novo_as_` so Datalog and other parsers can coexist.

`ActionSemantics.asdl` is authoritative for the source AST. Its generated visitor
calls `lower_expr` and `lower_action` on leave, so operands precede uses in
`Model.hpp`'s internal tables. The elaborator resolves all field indices, token
IDs, sort IDs, child evaluations and rules before returning a `Language`.
There is no public mutable instruction-table interface. Known scalar/token/cell
type incompatibilities fail elaboration; values from transients and bindings
have dynamic tags and are checked by the target runtime. Constructor fields
containing syntax nodes can only be evaluated, never forged from integer data.

All constructor names are global, and there is one semantic function (evaluation)
with one equation per constructor. Multiple mutually referring syntax sorts
are supported. A finite-term productivity fixed point rejects uninhabited sorts.
Rules may be written in any order, after the syntax declarations. Source order
determines IDs, and identical source always generates identical output.

`Generate.cpp` emits target-native immutable tables. The four `runtime.*.in`
templates implement the execution contract in README. They are embedded into
the host library at configure time; no runtime resource files are required.
C and Pascal use bounded arrays; Python and Rust enforce the same capacities
on their dynamic collections. All backends distinguish integer, token and cell
data. Syntax nodes are a separate domain. Cells begin uninitialized, are only
allocated by the allocation primitive, and can store only integers.

Target readers validate a term's constructor, sort, arity, integer bounds,
identifier syntax, nesting, trailing input, and byte/node/symbol budgets before
execution. Evaluation separates semantic failure/escape from resource failure.
Handlers do not catch resource failure, and storage is never implicitly restored.
Result binding compatibility is tested before result capacities to avoid
target-dependent error precedence at capacity boundaries.

Target source contains only fixed templates, validated ASCII identifiers, and
checked numeric tables. There are no source includes, foreign code snippets,
template expressions, shell commands, or raw string escapes in `.acsem`.
LaTeX identifiers and rendered equations are escaped; comments and source paths
are not copied into commands. Compilation of outputs is the caller's operation.

## Verification and maintenance

`tests/frontend.cpp` exercises diagnostics, malformed specifications, sort and
equation checks, generated output determinism, type errors, parse limits,
concurrent calls, and co-linkage with the Datalog Aurocks parser. It uses explicit
checks, including in Release builds.

`tests/backends.py` compiles each emitted language and runs the same semantic
corpus, including an independent arithmetic oracle. It also synthesizes distinct
minimal, multi-sort, and resource-test languages. The corpus observes binding
scope, transient routing, cell identity, persistent effects, failures, escapes,
overflow, malformed terms, and resource error precedence. C outputs are compiled
with strict warnings; sanitizer builds propagate their C flags into the generated
C compiler command. Pascal outputs run with range, overflow, and I/O checking.
The LaTeX test compiles the emitted document to PDF with shell escape disabled.

Runtime changes must update all four implementations and the semantic contract.
The backend corpus is the cross-target regression gate. Finite testing does not
constitute a proof that this dialect implements every rule of full Action Notation.
