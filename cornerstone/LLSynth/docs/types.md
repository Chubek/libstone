# Action types and symbol aliases

Products and sums describe values in semantic actions, states, symbol tables,
and helper functions. Here is a complete specification:

```text
.language "Coordinates"
.token NUMBER: %Int = /[0-9]+/

.prodtype %Point
    X: %Int
    Y: %Int
.end

.sumtype %Selection
    Found: %Point
    Missing: %Unit  // no information beyond the variant
.end

.alias %Selection %Result

.function score(item: %Result) -> %Int
    if %Result.is_Found(item) {
        let p = %Selection.get_Found(item);
        return %Point.X(p) + %Point.Y(p);
    }
    return 0;
.end

.rule Start
'syntax: n:NUMBER EOF
'semantics: return score(%Result.Found(%Point($n, 2)));
.end
```

Generate with `perl scripts/llsynth.pl --mode=interpret -o coordinates.py coordinates.llsynth`.
Input `40` prints `42`.

## Construction, inspection, and equality

| Expression | Meaning |
| --- | --- |
| `%Point(x, y)` | Construct a product, in declared field order. |
| `%Point.X(p)` | Read field `X`. |
| `p.X` | Read the same field with dot access. |
| `%Selection.Found(p)` | Construct a sum with the `Found` tag and its one payload. |
| `%Selection.Missing(nil)` | Construct a variant with a unit payload. |
| `%Selection.is_Found(item)` | Test whether the tag is `Found`. |
| `%Selection.get_Found(item)` | Read the payload; a different active variant raises a `SEM` error. |
| `item.Found` | Read the same payload with dot access. |

Each declaration introduces a distinct **nominal** type. An identically shaped
type declared elsewhere does not match it. Declarations can refer forward and
can be recursive, e.g. `Children: Vec<%Tree>`. A product can have zero fields;
a sum must have at least one variant. Member names are unique and cannot
conflict with generated operations (for example `Foo` and `get_Foo` in a sum).
Malformed declarations, unknown members or types, and statically known argument
type/arity mismatches are errors during `--check`, including in unused declarations.

Field and variant bindings are immutable. Reading a stored vector still returns
that vector. Equality compares the constructor and its payload values.
Constructors and accessors are themselves typed function values:
`let make = %Point; let p = make(1, 2);`. As elsewhere, `Any` bypasses
static operand type checking; accessors still check the owning type and tag
before reading a field. Python and native tools implement the same operations.
The public C interpreter entry point still requires a scalar or unit *final*
result; data can be used freely inside native actions.

## Unit and curried functions

`%Int`, `%Int64`, `%Bool`, `%Float64`, `%String`, and `%Any` are equivalent
to the existing unprefixed types. `%Unit` names the existing singleton `Void`
type: `nil`, `None`, `%Unit()`, an empty `return;`, and falling through a unit
function all yield its sole value. `%Unit()` and `fn() -> %Unit` have zero
arguments; a function taking one `%Unit` argument must be called with `nil`
or an equivalent unit value.

`%Callable[A -> B -> R]` is `Fn(A)->Fn(B)->R`: arrows associate to the right.
The first call supplies `A` and returns a unary function taking `B`. More
arrows add more unary calls; `%Callable` requires at least an input and a
result type. Use `Fn(A,B)->R` for an ordinary two-argument call. For
callables as arguments, nest brackets: `%Callable[%Callable[%Int -> %Int] -> %Int]`.

```text
.function add(x: %Int) -> %Callable[%Int -> %Int]
    return fn(y: %Int) -> %Int { return x + y; };
.end
```

In an action, `let f: %Callable[%Int -> %Int -> %Int] = add; return f(40)(2);`
returns `42`. Callable notation works on every target; constructing a capturing
`fn` closure, as in this example, requires the Python target (see
[execution.md](execution.md)). A named function can also return a named
function reference without a closure.

## `.alias SOURCE NEW`

The existing symbol comes **first**; the new spelling uses the same category
and sigil. This is a binding, not a copy:

```text
.alias %Point %Coordinate           // includes its constructors and fields
.alias @counter @count              // the same state slot
.alias #cache #memo                 // the same table
.alias !Missing !NotFound           // the same exception
.alias Number Numeric               // rule or token/literal
.alias score evaluate               // function; &score &evaluate also works
.alias twice double                 // template or macro
.alias Stack Bytecode               // IR namespace, including its op names
.alias Stack.PUSH pushOp            // one IR constructor in actions
.alias %Selection.get_Found %Unwrap // one sum or product operation
.alias %Selection.Found found       // ordinary callable spelling for the constructor
.alias %Point point                 // ordinary callable spelling for the product
.alias len size                     // size(v) calls the builtin len(v)
```

Aliases apply to referenceable specification declarations, not action-local
bindings or capture labels. They may precede their sources or name another
alias. A qualified source must be exported by the imported module. Aliases
can themselves be exported and then accessed via the importing scope, e.g.
`.alias %Math__Point %Coordinate`. An alias for a scalar type such as
`.alias %Int %Count` also works. Unknown or ambiguous sources, mismatched
sigils, duplicate bindings, and alias cycles are errors. Quoted strings,
token regexes, and messages are unaffected.

A callable constructor or accessor can use an ordinary alias (as above) or
keep its `%` spelling. Aliasing a namespace makes its members available; an
alias of an individual IR instruction is an action expression spelling, not
a new opcode in `.match`, `.replace`, or `.emit` blocks. Builtin aliases retain
the builtin's normal call syntax and arity; e.g. call `size(v)` directly,
not `&size(v)`. `.alias` does not convert a builtin into a named function or
an ordinary multi-argument function into a curried one.

A legacy `.type %New = %Existing` is transparent: if `%Existing` is a product
or sum, it also exposes its constructors and projections through `%New`.
For example `.type %P = %Point` allows both `%P(1, 2)` and `%P.X(p)`.
A member such as `%Selection.Found` is a callable value, not a type; use
`.alias %Selection.Found found` to rename it.
