# ASDL logistics runtime contract

`asdl.logistics(node, ...)` dispatches on the node's declared type and
constructor. Nested calls inherit the current phase and pass their own
inherited arguments positionally. Results preserve Lua arity, including
interior and trailing `nil`s. A declared hook traverses explicitly by
default. When no hook is registered for a type, the runtime visits all its
immediate AST children in field/sequence order and returns the original node.
It does not interpret analysis results as tree edits.

`asdl.run_logistics(options, node, ...)` starts an execution:

| Option | Default | Meaning |
|---|---|---|
| `phase` | `"default"` | Dispatch phase, shared by all recursive calls. |
| `context` | absent | Pass-level value visible as `asdl.logistics_context().value`. |
| `depth_limit` | `96` | Maximum active AST dispatch/refinement depth; supported range `1..256`. |
| `transform` | `false` | Require the first result to be a live AST node of the input root's declared type. |
| `refinements` | `false` | Evaluate executable field refinements before dispatch and on a changed transformation result. |

`asdl.logistics_children(node, ...)` explicitly dispatches on the immediate
children, returning an array of packed result tuples. Its children receive
the supplied inherited arguments as-is. Hooks can instead use
`asdl.map_children` when child results should replace AST fields.

`asdl.refine(node)` runs only the executable refinements. Nominal refinements
do not execute. Each executable refinement resolves to the named signed
export registered for the field and receives `(value, parent, field_name)`.
It runs once per present scalar or sequence element, after visiting any AST
descendants. Return `true` to accept, `false` or `nil` plus an optional reason
to reject, or a live replacement node of the same declared type. Replacement
descendants are checked before the new tree is returned. A refinement cannot
emit diagnostics, write annotations, or dispatch logistics.

Execution returns `nil, reason` and does not commit staged annotations or
provenance on checked failure, a new error/fatal diagnostic, or a missing
transformation root. Diagnostics themselves are retained. Successful
analysis may return zero results or `nil` values. Structural changes are
immutable; `asdl.origin(replacement)` can recover the immediately derived-from
node, while `asdl.location(replacement)` is the host location API. Semantic
annotations are not automatically copied to replacements. The transaction
does not roll back external side effects or mutation *inside* user-provided
Lua tables; treat committed annotation payloads as immutable.

## Embedding

The C++ `asdl::push_execution` installs the embedded Lua runtime on an active
`asdl` global after the AST bridge and signed-library loader. It registers
every schema hook, including non-unit types, and leaves the private
controller on the Lua stack. Schema Lua is text-only, is evaluated with the
schema's declared signed imports, and can use a small set of standard
utilities. Source Lua and library code are trusted extension code, not a
resource-isolated sandbox.

The controller's `register(spec, fn)` also supports per-constructor `tag`
(overrides the type-wide callback), optional inherited/synthesized name
lists to check arity, `traversal = "children"` (append packed child results
after inherited values), and `memoize = true` for pure callbacks. Memoization
is bounded and local to one run, keyed by node and inherited argument
identity; it does not snapshot mutable inherited objects. Memoized callbacks
must not mutate inherited values, write annotations, emit diagnostics, or
call impure hooks. These controller features do not imply syntax for every
option in the schema grammar.
