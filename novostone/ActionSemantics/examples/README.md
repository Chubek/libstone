# Executable extended examples

Synthesize the `.acsem` file with any of `--target c|python|rust|pascal`, compile
where appropriate, and feed the matching `.term` file to the generated program.
All examples automatically select dialect 2; `--target latex` renders equations.

| Specification / term basename | Expected output | Demonstrates |
| --- | --- | --- |
| closures | `completed 42` | Call-by-value lambda calculus, nested lexical closures, explicit currying |
| functions | `completed 3628800` | Factorial using letrec; also mutual recursion and tail calls |
| continuations | `completed 42` | Escaping saved control, shared store, second-resume rejection |
| generators | `completed 1 20 1 22 0 99 0` | Two yields, return value, terminal iteration |
| imperative_language | `completed 5` | Heap mutation, while, continue and break |

`tests/backends.py` additionally exercises scope shadowing, higher-order calls,
mutable captures, 100000 tail calls, 10000 generator suspensions, nested/recursive
generators, invalid scopes, normal/failure/escape propagation and exhaustion.
Tests compile and execute the same observations on all four backends.

No runnable examples for objects/modules/actors/promises/effects are supplied:
those phases are unimplemented, not pseudocode disguised as executables.
