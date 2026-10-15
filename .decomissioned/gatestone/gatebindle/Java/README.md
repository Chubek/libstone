# Java frontend reference bundle

Expression seed for Java with Java's three shifts: `<<` lowers to `shl`,
`>>` to arithmetic shift (`ashr`), and unsigned `>>>` to logical shift
(`lshr`). Remaining layers match the C++ bundle: assignment, bitwise,
additive, multiplicative, unary, calls, variables, and grouping. Comments
are `//` and `/* */`.

The seed uses signed `i64`; Java `int`/`long` widths, classes, generics,
and statements are future language extensions. `Assign`, `Variable`,
`Call`, and `CallEmpty` bind facility names for later backends.
