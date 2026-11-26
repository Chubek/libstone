# Native Lua AST host

`asdl::LuaSession` (`asdl/lua.hpp`) owns a Lua VM and a copy of the schema. Each
session has a separate arena, Lua globals, diagnostics, and options. `run`
executes a text chunk and `check` compiles it without executing it. C++ callers
can register trusted components with `extend`; the installer receives the
`asdl` table and a lightuserdata context and reports errors through Lua or C++
exceptions. Install the library loader before any component that imports ASDL
libraries. The AST host alone does not provide `asdl.lib` or `asdl.logistics`.

Nodes are full Lua userdata. The VM collects unreachable nodes, including
cycles through annotations. Lua `==` and `asdl.same` compare identity, and
`asdl.id` returns a session-local integer that is never recycled. `asdl.kind`
returns `product` or `sum`; `is_product`, `is_sum`, `is_enum`, `isnode`, and
`same` accept arbitrary Lua values. Fields and `destruct` are ordered:
constructor or product fields first, then sum attributes. `get` errors for an
undeclared field; `has` is false for an undeclared or absent optional field
but true for present false and empty sequences. Sequence tables are copied at
both API boundaries; editing a returned sequence does not edit the node.

`make(type, tag, named_fields [, origin])` accepts a source-location record or
an existing node as origin. `construct(type, tag, ...)` takes positional
fields. Products use a nil tag. Optional fields default to nil, `*` fields
default to an empty dense sequence, and `+` fields need at least one element.
`set(node, field, value)` updates the original node atomically, validating
type, cardinality and acyclicity. `replace(node, field, value)` returns a new
node; `copy` is shallow, `clone` deep and preserves shared subtrees. These
operations keep source location and record the source node's ID as
`provenance(new_node).origin`, but do not copy semantic annotations.
`map_children` visits direct node-valued fields and qualified sequences in
order. A callback returning nil or the same node leaves that position
unchanged. If there are no replacements, it returns the original node.

`equal` and `hash` ignore identity, annotations, and provenance. A hash is a
Lua integer for in-process use and is not a serialization format. `freeze`
marks the reachable AST structurally read-only; annotations remain mutable.
`annotation` reads a key and `annotations` returns a shallow mapping
snapshot. `schema`, `describe`, and `constructors` return fresh descriptor
tables, including field types, cardinalities, and schema-source spans.
`location` returns a copied source record or nil. `diagnostic.emit` retains a
copy of the diagnostic record; `diagnostic.records` returns a snapshot, and
`diagnostic.count([severity])` counts records without clearing them.

The host supports ASDL node types, primitive strings, booleans, integers
within the Lua integer range, finite Lua numbers, and field cardinalities.
Legacy C container/opaque handle types are not mapped into Lua values.
Constructor fields that share a name with an attribute are rejected by the
Lua host rather than silently overwriting one field in the named API.

The vendored engine is **Lua 5.5.0**, used with vendored Kaguya compatibility
helpers. The session also uses APIs available with Lua 5.4 and its tests were
run separately against system Lua 5.4.9. The supported production build
compiles vendored Lua as C++ so protected Lua errors unwind C++ frames,
force-including `src/lua_vendor.hpp` to keep the API's C linkage.
The default Lua script environment omits package, debug, coroutine, process
and direct filesystem operations. Allocation and instruction budgets are VM
limits, not an operating-system sandbox; trusted C extensions and expensive
native calls do not run under a Lua instruction counter. The traversal limit
bounds recursive native AST operations independently of VM stack limits.
