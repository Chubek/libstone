# Arithmetic in Amalgam8

`Arithmetic.am8` declares four prefix forms:

```text
(literal -12)
(variable x)
(plus EXPR EXPR)
(times EXPR EXPR)
```

`ArithmeticExtra.am8` adds `(twice EXPR)` with a second vocabulary table.
The arithmetic callbacks are generated from Amalgam8 syntax rules, and the
bounded parser in `Runtime.am8` looks up form names and arities in the tables.
`Example.am8` composes the tables. To add another operation, supply a
callback and table entry in an imported `.am8` module: the parser and
compiler need no changes. The same syntax rules also produce the optional
fixed-expression helper `arithmetic_eval(int x)` (use `x` from −1000 to 1000 for
that helper).

The generated C API is:

```c
int arithmetic_interpret(const char *program, long long x,
                         long long *out, const char **error);
```

Pass a NUL-terminated program, a value for `x`, and a result pointer.
The function returns 0 on success or 1 on error; it leaves `*out` unchanged
on error. If supplied, `*error` becomes either `NULL` or a static diagnostic.
Programs are limited to 1024 bytes and 32 nested expressions. Literals, `x`,
and every intermediate result must be between −1,000,000 and 1,000,000.

With the Amalgam8 compiler (`amalgam8` below) on `PATH`, run these commands
from this directory:

```sh
amalgam8 --api Example.am8
cc -std=c11 -x c -o arithmetic-api - <<'EOF'
#include "Example.hpp"
int main(int argc, char **argv) { return arithmetic_main(argc, argv); }
EOF
amalgam8 --cli Example.am8
c++ -std=c++20 Example.cpp -o arithmetic-cli
program='(twice (plus (literal 2) (times (variable x) (literal 10))))'
./arithmetic-api "$program" 2
./arithmetic-cli "$program" 2
```

Both print `44`; the CLI uses `x = 0` when no value is supplied. Passing
`'(times (literal 2))'` to either executable reports `error: expected '('`
with status 1. The CLI adapter delegates interpretation to the same
`arithmetic_interpret` function that C consumers can call directly.
