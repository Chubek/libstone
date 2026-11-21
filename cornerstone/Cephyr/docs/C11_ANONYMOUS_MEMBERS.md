# C11 anonymous structure and union members

Cephyr supports tagless `struct { ... };` and `union { ... };` declarations
inside records (§6.7.2.1p2,p8,p13). Their names are exposed recursively to the
containing record; duplicate exposed names are diagnosed. A tagged definition
or a typedef name without a declarator does not declare an anonymous member.

The anonymous subobject keeps its own size, alignment, padding and storage
rules. For example, fields in an anonymous structure inside a union do not
overlap each other. An enclosing union still overlaps that whole structure
with its other alternatives. `.` and `->`, address constants and `offsetof`
follow the resulting physical member path; member access inherits qualifiers
through all anonymous levels. Positional, braced and designated initializers
use the same physical nesting, including continuation after a promoted
designator and switching anonymous union alternatives.

The following limits still apply:

- Bit-fields are not supported, named or unnamed. GNU/MS extensions for
  anonymous members declared using a tag or typedef are not supported.
- Records containing an anonymous member, including inside a named record or
  array member, are rejected at native by-value call boundaries. This does not
  affect pointer parameters or results, local copies or static storage.
- The existing flexible-array restrictions apply to anonymous subobjects:
  a structure cannot contain a structure with a flexible array (directly or
  via a union). A preceding member promoted from an anonymous subobject does
  count as a named member for a containing structure's own flexible array.
- Existing volatile aggregate restrictions still apply, including overlapping
  designators in volatile automatic initialization and whole-object copies of
  unions with volatile members.
