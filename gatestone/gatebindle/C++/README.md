# C++ frontend reference bundle

Expression seed for C++, following the same packaging pattern as the C
bundle. Covers decimal integer arithmetic with C++ precedence: assignment
(right-associative), `|`, `^`, `&`, shifts, additive, multiplicative, unary
signs, `~`, single-argument and zero-argument calls, variables, and
parenthesized grouping. Comments are `//` and `/* */`.

`Shr` (`>>`) lowers to arithmetic shift (`ashr`) under the seed's signed
`i64` model. `==`, comparisons, templates, declarations, and statements are
future language extensions. `Assign`, `Variable`, `Call`, and `CallEmpty`
bind facility names (`assign`, `variable`, `call`, `callempty`) for later
backends; embedding applications supply those hooks.
