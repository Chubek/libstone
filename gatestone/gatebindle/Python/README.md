# Python frontend reference bundle

Expression seed for Python with `#` comments and Python's `**` power
operator (right-associative, binding tighter than unary minus, as in
`2 ** 3 ** 2` and `-2 ** 2`). Includes `//` floor division, `/`, `%`,
bitwise operators, shifts, assignment expressions, calls, variables, and
grouping. There is no indentation handling in this seed; every sample is a
single expression.

Semantic approximations under the signed `i64` model: `/` and `//` lower
to truncating `sdiv` rather than true/floor division, and `%` lowers to
`srem` rather than Python's floored modulo. `Power` binds the `pow`
facility for later backends; lowering it without a host hook is a located
error. Arbitrary-precision integers, strings, collections, and statements
are future language extensions.
