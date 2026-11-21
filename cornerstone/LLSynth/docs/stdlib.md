# Bundled LLSynth libraries

Import `std.Name` in a specification to use declarations from
[`stdlib/Name.llsynth`](../stdlib/). The generator finds the bundled directory
relative to itself; there is no need to pass `--module-path`. The generated
tool contains the linked declarations and can run without the `.llsynth`
files. None of these libraries installs tokens, skip rules, or global state.

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
let squares = F__map($numbers, &square);
return F__fold(squares, 0, &add);
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
or qualify them when using an alias (`.import "std.Functional" as F`, `F__map`).
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
| [Parsec](../stdlib/Parsec.llsynth) | `%Parser`, `token`, `satisfy`, `then`, `choice`, `many`, `complete`. Token-stream recognizers; see below. Requires the Python tool target for lexical closures. |
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

The libraries other than Parsec use portable action-language operations. On
native tool targets, pass named function references to higher-order functions
(as in `F__map($numbers, &square)`); Python tools can also pass closures.

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
* `many(p)` consumes zero or more consecutive matches, stopping on failure or
  no progress. `complete(p, tokens)` starts at zero and requires full
  consumption.

For example, with `.import "std.Parsec" as P` and `words:WORD* EOF` in a rule,
this action accepts one `begin`, any number of `item`s, then one `end`:

```text
'semantics:
let body = P__many(P__token("item"));
let clause = P__then(P__token("begin"), P__then(body, P__token("end")));
return P__complete(clause, $words);
```

These recognizers run in actions over tokens captured by your grammar. Their
return values describe recognition, not a parse tree.

Run `python3 cornerstone/LLSynth/tests/stdlib/test_stdlib.py` for end-to-end
Python and C examples, including malformed input and backtracking.
