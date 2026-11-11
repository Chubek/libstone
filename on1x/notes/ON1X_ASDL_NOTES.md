# on1x ASDL: schema notes

This file accompanies `on1x.asdl`, an expanded abstract-syntax schema based on
an initial `program`/`statement` excerpt and the attached `asdl.pl(1)` manual.
It is a proposed *AST schema*, not a complete lexer, parser, type checker, or
formal semantics for a previously specified on1x language.

## Code generation

Run from a repository containing the documented `asdl/asdl.pl` generator:

```sh
perl asdl/asdl.pl --check on1x.asdl
perl asdl/asdl.pl --check --schema on1x.schema.json on1x.asdl
perl asdl/asdl.pl --header on1x.h --visitor on1x_walk.h on1x.asdl
# For a split C11 implementation instead:
perl asdl/asdl.pl --header on1x.h --source on1x.c on1x.asdl
```

The generator itself was not attached, so those commands have **not** been run.
A local independent structure audit of `on1x.asdl` found 55 definitions
(9 products and 46 sums), 337 globally distinct constructors, no undefined
field types, no duplicated explicit field names in a field list, and no
products that depend on themselves transitively. This is not a substitute for
`asdl.pl --check` or a C compilation test with the actual generator.

## Compatibility with the original excerpt

- Preserved existing statement constructor names and field names except for
  necessary repairs listed below.
- `ImportStmt()` is replaced by `ImportStmt(import_spec spec)`.
- `FuncDefStmt()` is replaced by `FuncDefStmt(function_def definition)`.
  The original empty `()` syntax is not legal for nullary ASDL constructors.
- The `MatchStmt` discriminant is now `expression`, rather than `statement`.
- `MatchStmt.default_arm` and `SwitchStmt.default_arm` are optional.
- Top-level statement/directive/docstring lists use `*` rather than `+` so
  empty translation units and units without directives or docs are legal.
  `eval_env` is optional because an evaluation environment need not be baked
  into the parsed syntax tree.
- The statement `assoc_dirs` attribute uses `*`, not `+`: undecorated
  statements are valid. The sum additionally has `assoc_docs` and `span`.
- There are no zero-field `Foo()` constructors. `BreakStmt`, `BlankStmt`,
  `ContinueStmt`, and similar constructors are bare alternatives.
- `BlockStmt` retains the original `statement+ stmt_blk` requirement. Use
  `BlankStmt` in a block if the parser wants to retain an empty-statement
  token; alternatively change that field to `statement*` to permit an empty
  block directly.

## Semantic checks not expressible in ASDL

The AST builder/validator guarantees *structural* validity, not complete
language semantics. Downstream analysis should verify, at minimum:

1. The counts of `InitStmt.init_lhs` and `InitStmt.init_rhs` agree, if init
   assignments are pairwise rather than broadcast assignments.
2. In `CompareChainExpr`, there are exactly one fewer operators than operands,
   and at least two operands.
3. A present `MatchStmt.default_arm` has constructor `DefaultMatchArm`, and a
   present `SwitchStmt.default_arm` has constructor `DefaultSwitchArm`.
   Similarly, expression matches use `ExprDefaultArm`.
4. A `TuplePattern`/`ListPattern` rest element is legal and placed as required
   by the parser, and bindings do not define duplicate names.
5. `ArrayType.length` and variant discriminants satisfy constant-expression
   requirements where appropriate.
6. Required return types, parameter ordering, varargs placement, generic
   constraints, ownership/borrow rules, and lvalue requirements are enforced.
7. Qualified identifiers, imports, labels, scopes, exports, and directive
   applicability resolve and obey the language's visibility rules.
8. Literal spellings/bases are parsed and bounds-checked; a `string` containing
   a numeric literal's source spelling is intentional so the AST does not
   silently lose precision.
9. No `environ` record is treated as a live runtime scope/closure. Its
   `environment_entry` list is metadata and optional initializers, not owned
   mutable evaluation state.

## Schema organization

The file includes source-location products, statement variants, match/switch
arms, loop descriptions, assignable variables, literals and expressions,
assignment operators and ownership kinds, patterns, typed declarations,
function declarations, imports, macros, directives, token trees and docstrings.
Recursive syntax categories are represented as sums, which meets the
restrictions documented for this generator; only nonrecursive categories are
products.

The additional operator families and language constructs are deliberate
proposals. A concrete on1x frontend may choose to remove unsupported forms or
add operators, but should keep the recursive sum/product discipline.
