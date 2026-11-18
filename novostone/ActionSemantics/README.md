# ActionSemantics

An executable language synthesizer based on the **Action Semantics** framework
developed by Peter D. Mosses and David A. Watt. An `.acsem` specification gives
abstract syntax and compositional semantic equations. The synthesizer emits:

| Target | Artifact |
| --- | --- |
| `c` | Standalone C11 interpreter, standard library only |
| `python` | Standalone Python 3.9+ interpreter |
| `rust` | Standalone Rust 2021 interpreter, no crates |
| `pascal` | Standalone Free Pascal/ObjFPC interpreter |
| `latex` | Complete LaTeX document with syntax and semantic equations |

`scripts/aurocks.pl` generates the specification parser from
`ActionSemantics.g`. `scripts/asdl.pl` generates the arena-owned AST **and the
visitor used by semantic elaboration** from `ActionSemantics.asdl`. All generated
host files stay in the build directory. Target interpreters contain their own
constructor-term reader, instruction tables, and action machine; they do not
depend on Novostone, Perl, a Python helper, or the specification at runtime.

## Build and use

From the LibStone repository root:

```sh
cmake -S novostone -B build-novostone -DCMAKE_BUILD_TYPE=Debug
cmake --build build-novostone --parallel
ctest --test-dir build-novostone --output-on-failure

build-novostone/ActionSemantics/acsem --check novostone/ActionSemantics/examples/expressions.acsem
build-novostone/ActionSemantics/acsem --target c novostone/ActionSemantics/examples/expressions.acsem -o expressions.c
cc -std=c11 -Wall -Wextra -pedantic expressions.c -o expressions
./expressions < novostone/ActionSemantics/examples/store.term
# completed 42
```

The other executable targets run the same input:

```sh
build-novostone/ActionSemantics/acsem --target python novostone/ActionSemantics/examples/expressions.acsem -o expressions.py
python3 expressions.py < novostone/ActionSemantics/examples/store.term

build-novostone/ActionSemantics/acsem --target rust novostone/ActionSemantics/examples/expressions.acsem -o expressions.rs
rustc --edition=2021 expressions.rs -o expressions-rust
./expressions-rust < novostone/ActionSemantics/examples/store.term

build-novostone/ActionSemantics/acsem --target pascal novostone/ActionSemantics/examples/expressions.acsem -o expressions.pas
fpc -oexpressions-pascal expressions.pas
./expressions-pascal < novostone/ActionSemantics/examples/store.term

build-novostone/ActionSemantics/acsem --target latex novostone/ActionSemantics/examples/expressions.acsem -o expressions.tex
pdflatex -halt-on-error -no-shell-escape expressions.tex
```

Without `-o`, synthesis writes to stdout. `--check` validates all syntax and
equations, returning zero on success. CLI errors return 2 and include source
line/byte-column for specification errors. An invalid specification is rejected
before opening an output file. No external compiler is executed by `acsem`.

## A small specification

```text
language Calculator {
  start Expr;
  syntax Expr = Num(value: integer) | Add(left: Expr, right: Expr);
  rule Num = give value;
  rule Add = (evaluate(left) and then evaluate(right))
             then give given(1) + given(2);
}
```

Its synthesized language accepts `Add(Num(20),Num(22))` and reports
`completed 42`. The **concrete syntax of synthesized languages is constructor
form**: `Constructor(field, ...)`, including `Constructor()` for nullary cases.
Integer fields use signed decimal notation and identifier fields use bare
identifiers. Whitespace is insignificant. There are no comments in program
terms. Constructors are case-sensitive and checked against the expected sort.

Specifications support `#` and `//` line comments. Identifiers match
`[A-Za-z_][A-Za-z_0-9]*`; string literals are quoted identifiers such as `"x"`.
All `syntax` declarations precede all `rule` equations. Sorts may refer forward
to other sorts. `integer` and `identifier` are built-in field sorts. Constructor
names must be globally unique; field names are local to their constructor.
There must be exactly one rule per constructor, and every sort must admit a
finite term. Rules refer directly to the constructor's named fields.

## Executable action notation, dialect 1

The machine separates **actions**, **data**, and **yielders**. Every action
receives a tuple of transients and an environment, and accesses shared storage.
Completion produces a tuple and bindings. Failure produces neither; escape
produces a tuple without bindings. Storage updates persist through all outcomes.

Data are tagged signed 32-bit integers, identifier tokens, or allocated cell
identities. Integer zero cannot masquerade as cell zero or token zero. Cells
hold integers and begin uninitialized. Cells live for one interpreter invocation.

### Yielders

| Syntax | Meaning |
| --- | --- |
| `42`, `-7`, `field` | Integer literal or integer/identifier syntax field |
| `"name"` | Identifier token |
| `given(n)` | One-based transient projection, `1 <= n <= 64` |
| `bound(key)` | Datum bound to the token yielded by `key` |
| `contents(cell)` | Integer stored in the yielded cell |
| `a + b`, `a - b`, `a * b`, `a / b` | Checked integer arithmetic |
| `a == b`, `a < b` | Integer comparison, yielding 0 or 1 |

Multiplication/division bind tighter than addition/subtraction; comparisons
have the lowest precedence and do not chain. Parentheses group yielders.
Negative integer literals are supported; there is no general unary minus.
Division truncates towards zero. Overflow, division by zero, absent transients,
unbound tokens, uninitialized cells, and dynamically incompatible tags cause
**semantic failure**. Statically incompatible types are rejected at synthesis.
Syntax-valued fields are used through `evaluate(field)`, never as scalar data.

### Primitives

| Action | Completed result / behavior |
| --- | --- |
| `complete` | Empty tuple and bindings |
| `fail` | Failure |
| `give y` | Singleton transient tuple |
| `check y` | Complete if the integer is nonzero, otherwise fail |
| `escape y` | Escape with a singleton tuple |
| `bind key to y` | Empty tuple and one token-to-datum binding |
| `allocate cell` | Fresh uninitialized cell as a transient |
| `store value in cell` | Update an allocated cell; give no transients |
| `evaluate(field)` | Perform the child's semantic equation with current inputs |
| `furthermore A` | Perform `A`; overlay its output bindings on received bindings |

### Combinators

All binary action combinators have equal precedence and associate left. Use
parentheses to make grouping explicit. `furthermore` binds to one primitive or
parenthesized action.

| Combination | Right input transients | Right environment | Completed transients | Completed bindings |
| --- | --- | --- | --- | --- |
| `A then B` | A's output | Original | B's output | Disjoint union of A and B |
| `A and then B` | Original | Original | A's output followed by B's | Disjoint union of A and B |
| `A hence B` | Original | A's output bindings | A's output followed by B's | B's output bindings |

These three execute left-to-right and propagate failure/escape immediately.
Disjoint union fails on any duplicate key, even if values agree. Output binding
compatibility is checked before output capacities; bindings precede transients
in result capacity checking. `hence` **replaces** the received environment.
Use `furthermore A hence B` for ordinary lexical extension and shadowing:

```text
rule Let = furthermore (evaluate(value) then bind name to given(1))
           hence evaluate(body);
```

`A trapping B` runs B only on escape, with escaped transients and the original
environment. `A otherwise B` runs B only on failure, with the original inputs.
Successful A bypasses both handlers. **`otherwise` is a deterministic dialect
extension**, not the nondeterministic `or` of full Action Notation. Neither
handler rolls storage back. Resource errors bypass both handlers.

### Program results and limits

Generated interpreters consume exactly one whole program from stdin:

- Exit **0**: `completed` followed by its transient data.
- Exit **1**: `failed`, or `escaped` followed by escaped data.
- Exit **2**: malformed input or an enforced resource limit; diagnostic on stderr.

Integers print in decimal, tokens as `token:name`, cells as `cell:0`, etc.
Output bindings/storage are internal; programs can explicitly give their values.

| Resource | Limit |
| --- | --- |
| Specification / program source | 65,536 bytes each |
| Identifier or program token | 64 bytes |
| Constructors / fields per constructor | 256 / 16 |
| Lowered expressions plus actions | 4,096 |
| Elaboration visitor depth | 128 |
| Program tree nodes / parse nesting | 2,048 / 128 |
| Runtime symbols / allocated cells | 256 / 1,024 |
| Transient tuple / binding map | 64 entries each |
| Action/yielder evaluations / nesting | 100,000 / 128 |

Literal tokens count towards the runtime symbol limit. Aurocks also has a
512-production recursion guard; deeply nested specifications can hit it before
elaboration. Runtime depth counts action and yielder calls, not just program
tree depth. Limits report resource exhaustion, not proof of divergence.

## Extended notation: dialect 2

New constructs automatically select dialect 2. `--dialect 2` can also run an
existing specification with extended budget policies; `--dialect 1` rejects
extended specifications. Both modes use the same model, lowering, and
explicit-frame runtime templates. Legacy semantic step/depth accounting is
preserved, but generated source is no longer byte-identical to the old runtime.

| Construct | Behavior |
| --- | --- |
| `closure A`, `abstraction A`, `give closure A` | First-class lexical closure |
| `apply f to (x, y, ...)` | Call with left-to-right evaluated arguments |
| `enact f` | Call with the current transient tuple |
| `tailcall f to (...)` | Discard the current function continuation and call |
| `return y` | Complete the nearest function (or top level) |
| `letrec "f" = closure A, "g" = closure B in C` | Shared recursive lexical scope |
| `callcc f` | Pass a one-shot abortive continuation to a closure |
| `resume k with y` | Restore captured control; keep current store |
| `unfolding A`, `unfold` | Recursive action scope and recursive substitution |
| `tailunfold` | Explicit tail restart of the nearest unfolding scope |
| `put y in cell` | Store any supported tagged value; `store` remains integer-only |
| `if y then A else B` | Select one branch by integer truth |
| `while y do A`, `break`, `continue` | Bounded-frame loops, function-scoped control |
| `generator A`, `yield_value y`, `next g`, `close g` | Lazy stackless generators |
| `data Sort = C(T, ...) \| D();` | Declare immutable runtime data after `syntax`, before `rule` |
| `construct C(x, ...)` | Check arguments and give a fresh variant |
| `deconstruct y` | Give a variant's fields as transients in declaration order |
| `match y [of Sort] with { P [when g] => A, ... }` | Run the first matching arm with scoped captures |
| `arity(y)`, `component(y, n)` | Inspect a variant; component indices start at 1 |
| `equal(x, y)`, `identical(x, y)` | Structural versus identity equality; return 0 or 1 |

`A`, `B`, `C` above are primitives or parenthesized actions. Functions receive
arguments as `given(1)`, etc.; use `bind`/`furthermore` for named parameters.
Bindings are captured by value; captured cells preserve shared mutable identity.
Nested closures implement currying explicitly, not automatic partial application.

Runtime sorts are separate from constructor-term syntax. Data fields can be
`integer`, `identifier`, `datum` (any value), or another data sort. Match
patterns can be `_`, a capture name, an integer, a quoted token, a nested
constructor, `P as name`, or `P | Q`. Use `bound("name")` to read a capture.
An optional `of Sort` checks the subject's runtime sort and requires the
compiler to establish coverage. See [SEMANTICS.md](SEMANTICS.md) for the
coverage check, guard behavior and equality rules. The
[pattern matching example](examples/pattern_matching.acsem) sums a recursive
list: `Sum(Prepend(20,Prepend(22,Empty())))` prints `completed 42`.

`next` returns `1, value` on suspension, `0, completion-data...` on termination,
and just `0` after termination/close. Failure and escape propagate. Generator
close discards suspended frames; it does **not** invoke cleanup handlers.
Call/cc and generators currently cannot share a specification: synthesis reports
a capability error. Multi-shot continuations, shift/reset and cleanup are not
implemented.

```sh
build-novostone/ActionSemantics/acsem --target c \
  novostone/ActionSemantics/examples/closures.acsem -o closures.c
cc -std=c11 -Wall -Wextra -Werror -pedantic closures.c -o closures
./closures < novostone/ActionSemantics/examples/closures.term
# completed 42
```

Dialect-2 budgets can be set at synthesis time with repeated
`--define steps=1000000 --define heap=128`, etc. See [RUNTIME.md](RUNTIME.md)
for limits and [BACKENDS.md](BACKENDS.md) for exact coverage.
The default C11 output still needs only the standard library.

## Library and installation

```cpp
#include <novostone/ActionSemantics/ActionSemantics.hpp>
using namespace novostone::action;
auto language = parse("language L { start E; syntax E = N(); rule N = give 42; }");
auto c_source = generate(language, Target::C);
```

Link `Novostone::ActionSemantics` after `find_package(Novostone CONFIG REQUIRED)`.
`Language` is an immutable, copyable validated model. Parsing serializes the
Aurocks globals; independent generation is read-only. `ParseError` exposes
source, offset, line, and byte column. `target(string_view)` resolves CLI target
names. Generated source includes dialect version and a deterministic FNV-1a-64
source fingerprint (provenance, not a cryptographic authenticity check).

`cmake --install build-novostone --prefix PREFIX` installs the library, header,
`bin/acsem`, specifications and examples. Installed consumers need neither Perl
nor the repository. Shared builds use relative runtime library paths.
`NOVOSTONE_BUILD_TOOLS=OFF` omits the CLI. `NOVOSTONE_BUILD_TESTS=OFF` omits tests;
tests with the CLI enabled require Python. Rust, Pascal, and LaTeX execution
tests are enabled when their tools are available, with configure-time notices
when unavailable. They are not dependencies of the library or C backend.

## Scope and references

This is a specified, executable subset of Action Notation. It implements basic,
functional, declarative, and imperative facets and selected exception handling.
It does not yet implement arbitrary concrete grammars, nondeterministic choice,
concurrent agents, or the complete notation from the book. Dialect 2 adds
procedural closures, recursive unfolding, one-shot continuations, synchronous
generators and a nominal algebraic data subset, not a universal runtime.
Generated artifacts
are interpreters for the described language, not optimizing native compilers
for individual input programs. Unknown notation is rejected.

- Peter D. Mosses, *Action Semantics*, Cambridge University Press, 1992.
- [Mosses: Action Semantics project and collaboration with David Watt](https://pdmosses.github.io/research/action-semantics/).
- [Theory and Practice of Action Semantics](https://www.brics.dk/RS/96/53/BRICS-RS-96-53.pdf), 1996.
- [DESIGN.md](DESIGN.md): implementation and dependency boundaries.
- [STATUS.md](STATUS.md): tested configurations and evidence.
