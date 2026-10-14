# Rust frontend reference bundle

Expression seed for Rust. Uses Rust's `!` for bitwise negation rather than
C's `~`. Remaining layers match the C++ bundle: assignment, bitwise,
shifts, additive, multiplicative, unary signs, calls, variables, and
grouping. Comments are `//` and `/* */` (non-nesting in this seed).

Integer suffixes, ownership types, patterns, traits, and items are future
language extensions. The seed uses signed `i64`. `Assign`, `Variable`,
`Call`, and `CallEmpty` bind facility names for later backends.
