# Bundled LLSynth libraries

Import `std.Name` in a specification to use declarations from
[`stdlib/Name.llsynth`](../stdlib/). The generator finds the bundled directory
relative to itself; there is no need to pass `--module-path`. The generated
tool contains the linked declarations and can run without the `.llsynth`
files. Libraries that call a native dependency may still need its runtime or
link library. None of the bundled libraries adds lexer rules.

For example, save the following as `sum-squares.llsynth`:

```text
.language "SumSquares"
.import "std.Functional" as F
.token N: Int = /[0-9]+/
.skip /[ \t\n]+/

.function square(x: Int) -> Int
return x * x;
.end

.function add(a: Int, b: Int) -> Int
return a + b;
.end

.rule Start
'syntax: numbers:N+ EOF
'semantics:
let squares = F:map($numbers, &square);
return F:fold(squares, 0, &add);
.end
```

For input `2 3`, `map` produces `[4, 9]` and `fold` adds them to give `13`.
From the repository root:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=interpret -T python3 \
    -o example.py sum-squares.llsynth
printf '2 3\n' | python3 example.py
```

## Library reference

Use the ordinary names below after an open import (`.import "std.Functional"`),
or qualify them when using an alias (`.import "std.Functional" as F`, `F:map(...)`).
Use `&F->map` to pass a function reference and `%P->Parser` to refer to an
exported type. Legacy `F__map(...)` calls remain supported.
The links point to the full typed signatures. Unless stated otherwise, vectors
contain `Int`. Functions producing vectors return new vectors; the algorithms
do not mutate their inputs. Integer operations use LLSynth's checked signed
64-bit arithmetic. Large computations are subject to the generated tool's
action budget.

| Library | Exports and behavior |
| --- | --- |
| [ISel](isel.md) | Adds `.isel` blocks to select concrete ArchML instruction forms and emit Sandstone `.insn` assembly from legalized IR. |
| [Functional](../stdlib/Functional.llsynth) | `map(items, transform)` applies an `Int -> Int` callback in order. `filter(items, keep)` retains values passing an `Int -> Bool` predicate. `fold(items, seed, combine)` visits left to right as `combine(acc, value)`; empty input returns `seed`. |
| [Collections](../stdlib/Collections.llsynth) | `slice(items, start, stop)` copies the half-open range within bounds; negative indices clip at zero, and reversed bounds give `[]`. `concat(a, b)` joins two vectors; `flatten(rows)` concatenates all rows in order. |
| [Parsec](../stdlib/Parsec.llsynth) | Composable token-stream recognizers with ordered choice, lookahead, and repetition. Requires the Python tool target for lexical closures. |
| [SExpr](../stdlib/SExpr.llsynth) | `%Expr`, `parse(tokens)`, `parseAll(tokens)`, `toTokens(expr)`, and `!Syntax`. Reads and writes proper S-expressions over string tokens. |
| [Sorting](../stdlib/Sorting.llsynth) | `sorted(items)` returns an ascending stable insertion sort. `merge(left, right)` merges **already sorted** vectors in linear time, keeping duplicates. |
| [Search](../stdlib/Search.llsynth) | On ascending input, `lowerBound(items, key)` returns the first index with value ≥ key; `upperBound` uses > instead. Both return the length if none qualifies. `equalRange(items, key)` gives `[lower, upper]`, the half-open span of duplicates. |
| [Sets](../stdlib/Sets.llsynth) | `unique(items)`, `union(left, right)`, `intersection(left, right)` return one copy per value. The first encounter determines order; the intersection follows the left input's order. |
| [Sequences](../stdlib/Sequences.llsynth) | `range(start, stop)` counts up to but excludes `stop`. `prefixSums(items)` includes an initial zero. `differences(items)` gives successive differences, so `differences(prefixSums(xs))` recovers `xs`. |
| [Statistics](../stdlib/Statistics.llsynth) | `mean(samples)` and `variance(samples)` return `Float64` and reject an empty sample. `variance` computes **population** variance (divides by `n`) with Welford's one-pass recurrence. |
| [Integer](../stdlib/Integer.llsynth) | `modulo(value, divisor)` produces a nonnegative remainder; `ceilDiv(value, divisor)` rounds toward positive infinity. Both require `divisor > 0`. `gcd(a, b)` requires nonnegative inputs; `gcd(0, 0)` returns zero. |
| [RunLength](../stdlib/RunLength.llsynth) | `encode(items)` emits alternating value/count pairs: `[2, 2, 5]` becomes `[2, 2, 5, 1]`. `decode(pairs)` rejects a missing or nonpositive count. Both map `[]` to `[]`. |
| [Graph](../stdlib/Graph.llsynth) | `distances(edges, source)` takes directed adjacency lists indexed by vertex ID; the result holds minimum edge counts and `-1` for unreachable vertices. `reachable(edges, source, target)` answers a reachability query using the same traversal. Source, target, and encountered neighbor IDs must be valid. |
| [Bytes](../stdlib/Bytes.llsynth) | `writeU32LE(value)` returns four little-endian octets for a value in `0..4294967295`. `readU32LE(bytes, offset)` reads four octets at `offset` and rejects truncation, invalid offsets, or selected octets outside `0..255`. |
| [Polynomial](../stdlib/Polynomial.llsynth) | Coefficients are in ascending degree order. `evaluate(coefficients, x)` uses Horner's method; `derivative(coefficients)` gives formal derivative coefficients. An empty vector represents zero. |
| [LinearAlgebra](../stdlib/LinearAlgebra.llsynth) | `dot(a, b)` requires equal lengths. `matvec(rows, vector)` takes one dot product per row. `transpose(rows)` swaps rows and columns, rejecting ragged input. An empty matrix or matrix with zero columns transposes to `[]`. |

The algorithm libraries in the table use portable action-language operations
except for Parsec's use of closures and ISel's generator-time integration.
On native tool targets, pass named function references to higher-order
functions (as in `F:map($numbers, &square)`); Python tools can also pass
closures.

### Host services and code analysis

These libraries are explicit imports. They make host operations and analysis
available to specification actions and passes; importing them does not
automatically change the generated language or enable optimization passes.

| Import | Purpose |
| --- | --- |
| [`std.ipc`](ipc.md) | Use pipes, FIFOs, local socket pairs, POSIX message queues, shared memory, and named semaphores. |
| [`std.shell`](shell.md) | Run shell source through the vendored libshell. |
| [`std.egraph`](egraph.md) | Analyze and rewrite equivalent terms with Equinox NG. |
| [`std.assembler`](assembler.md) | Assemble instruction text with the Sandstone embedding API. |
| [`std.isa`](isa.md) | Inspect instruction-set descriptions with Sandstone ArchML. |
| [`std.flowgraph`](flowgraph.md) | Analyze control-flow graphs with Wetstone data-flow facilities. |

#### Native build

Build the optional native bridges from the repository root:

```sh
cmake -S cornerstone/LLSynth -B build/llsynth-native
cmake --build build/llsynth-native --parallel
```

Each component can be disabled at configure time with `-DLLSYNTH_BUILD_<NAME>=OFF`,
where `<NAME>` is the uppercase suffix of its import. The generator itself
does not need CMake. For Python tools, put `build/llsynth-native/lib` on
[`LLSYNTH_LIBRARY_PATH`](interop.md#python-3-tools) when running an action
that calls a bridge, and put `build/llsynth-native/bin` on `PATH` for
`std.shell` to find `libsh`. `std.ipc` is embedded in the generated tool and
does not use a shared bridge. For C tools,
link the shared libraries for the imported bridges and make those libraries
available to the platform loader at runtime. See
[native C interoperability](interop.md#c11-tools-and-split-embedding).

#### Running an analysis pass

The [term analysis example](../examples/analysis/simplify.llsynth) uses an
`'analyze` facet to check liveness and reaching definitions for its evaluation
graph and to simplify a pure input term by equality saturation:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=analyze \
  -o simplify.py cornerstone/LLSynth/examples/analysis/simplify.llsynth
printf '(add (add value 0) 0)\n' |
  LLSYNTH_LIBRARY_PATH="$PWD/build/llsynth-native/lib${LLSYNTH_LIBRARY_PATH:+:$LLSYNTH_LIBRARY_PATH}" \
  python3 simplify.py --analyze
```

The report contains the extracted term `value`. The example supplies its
control-flow graph and rewrite rule explicitly; neither library guesses
semantics from operation names.

## Recognizing token sequences with Parsec

`%Parser` is `Fn(Vec<String>,Int)->Int`. A parser receives tokens and a starting
index and returns either the next index or `-1` on failure. A user-defined
parser should leave the input unchanged and, on success, return a cursor
between its starting index and the length inclusive. A zero-width success
is valid.

* `token("x")` consumes one exact token; `satisfy(predicate)` consumes one
  token if the string predicate accepts it.
* `then(a, b)` runs `b` at the next cursor from `a`. `choice(a, b)` takes the
  first success, retrying `b` at the **original cursor** on failure. Retrying
  rolls back the cursor only; keep custom recognizers free of side effects.
* `optional(p)` returns the original cursor if `p` fails. `ahead(p)` and
  `notAhead(p)` succeed without consuming input when `p` succeeds or fails,
  respectively.
* `many(p)` consumes zero or more consecutive matches, stopping on failure or
  no progress. `some(p)` requires at least one consuming match. `eof` succeeds
  only at the end; pass `&P->eof` as a recognizer. `complete(p, tokens)` starts
  at zero and requires full consumption.

For example, with `.import "std.Parsec" as P` and `words:WORD* EOF` in a rule,
this action accepts one `begin`, any number of `item`s, then one `end`:

```text
'semantics:
let body = P:many(P:token("item"));
let clause = P:then(P:token("begin"), P:then(body, P:token("end")));
return P:complete(clause, $words);
```

These recognizers run in actions over tokens captured by your grammar. Their
return values describe recognition, not a parse tree. Aurocks `%fault`
rules belong to Aurocks `.g` grammars; LLSynth libraries report malformed
input through action-language exceptions instead.

## Reading S-expressions

Import `.import std.SExpr as S` and capture a `Vec<String>` with your own
lexer. The tokens `"("` and `")"` delimit lists; each other nonempty token
is an atom. The library leaves whitespace, comments, quoting, and escaping
to the caller's lexer. It does not interpret numbers, quote shorthand, or
dotted pairs.

* `S:parse(tokens)` reads one complete expression and rejects trailing tokens.
  `S:parseAll(tokens)` reads zero or more complete expressions.
* `%S->Expr` is a nominal sum with `Atom: String` and `List: Vec<%S->Expr>`.
  Use `%S->Expr.is_Atom(value)` or `%S->Expr.is_List(value)` to test the tag,
  then `value.Atom` or `value.List` to read its payload.
* `S:toTokens(value)` emits a new token vector, without whitespace. Constructed
  atoms that are empty or equal to a delimiter are rejected. Reading and
  writing both allow up to 64 nested lists.
* Malformed data raises `!S->Syntax`; error messages give a zero-based token
  index, with EOF at `len(tokens)`.

The [atom-counting example](../examples/sexpr/count.llsynth) reads `(a (b) ())`
and returns `2`. It works with both Python and native generated tools.

Run `python3 cornerstone/LLSynth/tests/stdlib/test_stdlib.py` for end-to-end
Python and C examples, including S-expression round trips, malformed input,
and recognizer backtracking.
