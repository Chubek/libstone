# Ruby frontend reference bundle

Expression seed for Ruby with `#` comments and Ruby's right-associative
`**` power operator. Includes the bitwise operators, shifts, assignment
expressions, calls, variables, and grouping.

Semantic approximations under the signed `i64` model: `/` lowers to
truncating `sdiv` rather than Ruby's floored integer division. `Power`
binds the `pow` facility for later backends; lowering it without a host
hook is a located error. Blocks, symbols, dynamic dispatch, and statements
are future language extensions.
