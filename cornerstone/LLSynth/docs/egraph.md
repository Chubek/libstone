# Equality-saturation analysis with `std.egraph`

`std.egraph` runs the vendored [EquinoxNG](../../../third_party/equinox-ng)
e-graph on explicit, pure terms. It does not alter the parser, StackIR rewrite
pass, or generated code. Call it from an action such as `'analyze:`.

```text
.language "Simplify"
.import std.egraph as E
.rule Start
'syntax: EOF
'analyze: print(E:analyze("(add (add value 0) 0)", "(=> (add ?x 0) ?x)"));
.end
```

## API and term language

| Function | Arguments | Returns |
| --- | --- | --- |
| `analyze` | `term: String, rules: String` | JSON report including the best observed term |
| `equivalent` | `lhs: String, rhs: String, rules: String` | JSON report including the equality result |
| `analyzeWithLimits` | `term: String, rules: String, iterations: Int, nodes: Int` | Same as `analyze`, with explicit limits |
| `equivalentWithLimits` | `lhs: String, rhs: String, rules: String, iterations: Int, nodes: Int` | Same as `equivalent`, with explicit limits |

All functions return `String`. Terms look like `value`, `(zero)`, or
`(add value 0)`. An application starts with an operator atom. Bare atoms are
opaque; `0` is not an integer, and `zero` differs from `(zero)`. Unquoted
ASCII atoms accept letters, digits, underscore and `+*/%<>=.!:$&|~^-`.
Spaces, tabs and line breaks separate terms. There are no quoted atoms,
comments, or built-in rules.

Supply zero or more `(=> LEFT RIGHT)` rules, separated by whitespace. Rules
match left to right and **merge** the matched and instantiated classes; that
merge is symmetric, but the rule is not automatically applied right to left.
Use a second rule to enable reverse search where valid. In rules, `?name`
matches an e-class; repeat the variable to require the same class. A
left-hand side cannot be a bare variable, and every right-hand variable must
be bound on the left. Input/query terms cannot contain variables.

A successful report has `status`, `stop`, `term`, `cost`, `iterations`,
`enodes`, and `unions` fields. `term` is the cheapest observed finite
representative of the first input; `cost` counts its atoms and applications.
Equal-cost alternatives follow EquinoxNG's traversal order, not a lexical
canonicalization rule. `enodes` counts allocated graph nodes, including
entries retained by EquinoxNG after congruence closure. `unions` includes
both rewrite and congruence merges.

* `analyze` reports `saturated`, `iteration-limit`, `node-limit`, or
  `work-limit` in `status` and `stop`. A budget stop returns the best term
  observed so far, not a globally optimal result.
* `equivalent` reports `equivalent` if the two terms enter the same class
  and `unknown` otherwise. See `stop` for the search outcome. Even a
  `saturated` stop is not a proof of disequality.
* Invalid input returns `{"status":"error","message":"..."}`. Native
  loader and foreign type errors still use LLSynth's usual `INTEROP`
  diagnostics. The default CLI's `analysis OK` only means that the
  actions finished; callers must inspect the report.

Defaults are 12 rounds and 1,024 allocated e-nodes. Limits accept 1–32
rounds and 1–4,096 e-nodes. Search also has a fixed budget of 100,000
matching-work units per call.
Each input string is limited to 16 KiB. The reader accepts at most 64 rules,
32 levels per term, 64 bytes per atom/variable, 128 syntax nodes per query
term, and 48 syntax nodes per rule side. Input terms must fit the node budget
before search starts. These are resource bounds, not a wall-clock timeout.

Only supply equations sound for your term domain. The module does not prove
the rules, type-check operators or model side effects, arithmetic, traps,
floating-point behavior, or undefined values.

## Optional Linux bridge

Run from the repository root; save the specification above as
`simplify.llsynth`:

```sh
cmake -S cornerstone/LLSynth/stdlib/egraph -B build/llsynth-egraph
cmake --build build/llsynth-egraph
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=analyze \
  -o simplify.py simplify.llsynth
LD_LIBRARY_PATH="$PWD/build/llsynth-egraph${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  python3 simplify.py --analyze </dev/null
```

The build requires CMake and a C++20 compiler, with no network or separate
EquinoxNG installation. `LLSYNTH_EQUINOX_DIR` can point to another checkout
of the same headers. Python generation needs no native compiler or bridge;
only calls load `libllsynth_egraph.so`. The C99/C11 tool targets instead
reference the C ABI directly. For example, with `-T c11 -o simplify.c`:

```sh
cc -std=c11 simplify.c -L build/llsynth-egraph \
  -Wl,-rpath,"$PWD/build/llsynth-egraph" -l llsynth_egraph -lm -o simplify
./simplify --analyze </dev/null
```

The generated C++ tool targets do not support LLSynth's native C import
syntax. Tools that do not import this module do not need any EquinoxNG
dependencies.

```sh
python3 cornerstone/LLSynth/tests/stdlib/test_egraph.py
```
