# Evaluation, rewriting and contification

## Operational meaning

`Evaluator` implements a reference call-by-need machine:

- The value environment contains shared, memoized thunks. `let` allocates a
  thunk and does not force it; `let rec` creates a mutually recursive group.
- A lambda value is a closure; applying it binds its argument to a thunk.
- Constructor fields are lazy and memoized after forcing.
- A join is a lexical command closure. `jump` binds thunks for its parameters
  and enters the named block without installing a function-return frame.
- `Mu` discards the current join namespace and runs its body with the supplied
  dynamic continuation, which enforces a local exit point.
- The evaluator reports recursive black holes and fuel exhaustion as errors.

Use `Evaluator::run(module)` on a verified, closed module. This is **not** a
JIT/AOT implementation and does not implement general foreign function calls.

## Rewriting

`Transform.hpp` implements persistent (copying) rewrites, currently:

1. Mu reduction using scoped continuation substitution.
2. Beta reduction using fresh `let` bindings, preserving argument sharing.
3. Known Boolean/constructor case reduction.
4. Dead value/label binding elimination (for nonrecursive bindings).
5. A conservative, type-informed **contification** pass.

`RewriteStats` reports the number of simplification passes and a hard limit
flag. `optimize_checked` validates before and after; a failed proof obligation
is an exception, **not** silently accepted code.

## Conservative contification preconditions

For `let f = lambda x. mu ret. c in body`, conversion to `let join` is safe
only when every use of `f` in the body is a fully saturated **tail** cut;
`f` is not captured by any value/closure; its body does not recursively invoke
`f` as an ordinary variable; and the result type is *exactly* the enclosing
return type. Nested `Mu` computations are checked separately. Recursive
function contification is deliberately not yet enabled.

## Known work for production use

The paper's *shrink(k)* procedure protects against code duplication when a
large continuation is pushed through a many-way case. This initial pass
copies alternative continuations and has no cost model; use it with budgets
and bounded program sizes. Suggested next milestone is a typed, capture-free
shrinking pass that creates a shared join rather than duplicating a suffix.
A production implementation also needs alpha-renaming hygiene, costed
inlining, stable source IDs, pipeline integration, and multi-target lowering.
