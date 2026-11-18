# Backend coverage

Legend: **Yes** = executable and tested; **partial** = narrower stated subset;
**No** = unavailable, not implemented as successful-looking stubs.

| Facility | C11 | Python | Rust | Pascal | LaTeX |
| --- | --- | --- | --- | --- | --- |
| Legacy notation | Yes | Yes | Yes | Yes | Equations |
| Explicit-frame actions / tracing GC | Yes | Yes | Yes | Yes | Contract |
| Lexical escaping closures / mutable cell capture | Yes | Yes | Yes | Yes | Equations |
| Multi-argument apply, enact, letrec, return, tailcall | Yes | Yes | Yes | Yes | Equations |
| One-shot abortive callcc/resume | Yes | Yes | Yes | Yes | Equations |
| Unfolding, unfold, tailunfold | Yes | Yes | Yes | Yes | Equations |
| Integer if/while, scoped break/continue | Yes | Yes | Yes | Yes | Equations |
| Synchronous lazy generators, next/close | Yes | Yes | Yes | Yes | Equations |
| Rich values | partial | partial | partial | partial | Contract |
| Currying / partial application | partial | partial | partial | partial | Equations |
| Typed action validation | partial | partial | partial | partial | Equations |
| Callcc combined with generators | Error | Error | Error | Error | Error |
| Multi-shot, shift/reset, effects, finally/defer | No | No | No | No | No |
| Nominal recursive ADTs, selectors and equality | Yes | Yes | Yes | Yes | Equations |
| Nested, guarded, or/as action patterns | Yes | Yes | Yes | Yes | Equations |
| Pattern exhaustiveness and reachability analysis | partial | partial | partial | partial | Contract |
| Yielder matches, built-in collections, objects, modules, richer types | No | No | No | No | No |
| Tasks, promises, channels, actors, parallelism | No | No | No | No | No |
| Search, transactions, reactive execution | No | No | No | No | No |
| Foreign calls / external capabilities / libuv | No | No | No | No | No |
| User-defined concrete grammars | No | No | No | No | No |

Default artifacts use only each target's standard library. Rust uses edition 2021,
Pascal ObjFPC; Python has no asyncio dependency. The constructor-term frontend
remains supported. Unknown syntax fails parsing; the known unsafe combination
above fails generation with a capability diagnostic. No proposed `--runtime`,
`--scheduler`, or `--features` flags are accepted yet.

Rich values currently means i32/token/cell/closure/continuation/generator and
immutable user-defined variants. Pattern analysis covers annotated sum sorts
conservatively and diagnoses provable single-arm subsumption.
Currying requires explicit nested closures; no automatic arity machinery exists.
Static validation checks known scalar tags; bindings/transients remain dynamically
checked. The Error row is a synthesis capability diagnostic.
