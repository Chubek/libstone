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
calls `leave_expr` and `leave_action` on leave, so operands precede uses in
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
data. Syntax nodes are a separate domain. Cells begin uninitialized and are
allocated by the allocation primitive. `store` accepts integers; `put` accepts
all implemented tagged values.

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

## Unified Action Machine

`Spec` retains one canonical `Model` from `Model.hpp`. `ActionSemantics.cpp` uses
the generated postorder visitor for all lowering, resolves operand-pool indices,
checks static yielder tags, and preserves diagnostic offsets. A preliminary
visitor detects tagged stores across all rules so integer-only specifications
retain their stronger `contents()` validation. Extended action nodes select the
required capability mode, not a second IR or runtime implementation.

`runtime.*.in` implement one explicit action-machine contract. An action frame
contains its instruction, defining syntax node, lexical environment, input tuple,
program counter state, function boundary, and rooted intermediate results.
Dispatch never recursively invokes another action. Pure yielders and constructor
readers still use bounded host recursion.

The public dialect selectors remain compatible: One restricts constructs and
retains legacy semantic entry/yielder step accounting and logical recursion
depth; Two enables extensions and charges frame delivery steps. A small mode
policy in the same machine handles those accounting differences. Tail-replaced
`evaluate` frames retain a logical depth for legacy budget checks. Generated
source and tables now use the unified representation in both modes, rather than
preserving old artifact bytes.

Data/environment objects are persistent. Closures capture their environment and
syntax node; the syntax tree lives for the full invocation. `letrec` creates one
shared environment and fills its closure entries before executing its body.
Cells retain identity and can hold arbitrary *implemented* tagged values through
`put`; legacy `store` still checks integers. Cells are monotonic, not reclaimed.
Tracing collection follows cells, closures, environments, tuples and saved frames,
including cycles. The C/Pascal worklists mark on enqueue to bound auxiliary space.

One-shot call/cc copies caller frames into a bounded heap chain, excluding the
capture action. Resume replaces control state, not store state. Generators use
the same saved-frame representation, but delimit suspension at `next` and resume
as empty completion of `yield_value`. Normal/failed/escaped generator completion
closes the generator. Reentrant next/close fails. Mixing call/cc with generators
is a synthesis-time capability error pending a sound cross-delimiter lifecycle.

`unfold` substitutes the nearest unfolding body without losing pending
combinators; `tailunfold` deliberately discards those pending frames.
Unfolding cannot cross an active generator delimiter. Conditional dispatch is a
tail replacement. While keeps one delimiter and restarts after body completion;
break/continue cannot cross a function boundary.

See [SEMANTICS.md](SEMANTICS.md) for transition rules and
[BACKENDS.md](BACKENDS.md) for unsupported portions of the expansion mission.
