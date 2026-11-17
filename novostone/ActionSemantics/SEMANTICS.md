# Extended operational semantics

Write `⟨A,n,ρ,d,σ,K⟩` for action, syntax node, lexical bindings, transient tuple,
shared store and explicit frame continuation. `done(d,b)` completes with data and
output bindings; failure and escape follow README's legacy equations. All
yielders are pure, evaluated left-to-right, with checked tags and budgets.
These are dialect rules, not a claim of full classical Action Notation.

## Functions

Let `c = closure(A,n,ρ)`. The three forms `closure A`, `abstraction A`,
`give closure A` all produce `done([c],{})`. Capture does not execute A.

```text
apply f to (e1,...,em):
  c = eval(f,n,ρ,d,σ); args = [eval(e1),...,eval(em)]
  execute c.A with c.n, c.ρ, args, σ, FunctionBoundary(K)
enact f:
  same, but args = d
return e:
  deliver done([eval(e)],{}) outside the nearest FunctionBoundary
tailcall f to args:
  evaluate f and args; discard frames through the nearest function boundary;
  execute the callee with that boundary and the surviving outer continuation
```

Top-level return/tailcall use the whole program as the function scope.
Applications accept up to 64 arguments without declared arity; missing `given`
projections fail. Bindings are immutable captured values. Captured cell handles
refer to the same store locations; mutation remains visible after scope exit.
Lexical rebinding overlays a new environment without changing existing captures.

For `letrec "f1"=closure A1,...,"fm"=closure Am in B`, allocate shared ρ′
initially containing ρ, overlay each fi with `closure(Ai,n,ρ′)`, then execute B
under ρ′ and original d. Duplicate names are a static error. The scope does not
implicitly export fi. Named parameters, currying and higher-order composition
are expressed with existing bind/closure/apply primitives; defaults and automatic
partial/variadic parameter declaration syntax are unsupported.

## Store

`put e in l` updates `σ[eval(l)] = eval(e)` and completes empty, provided l is an
allocated cell. `contents(l)` returns that tagged datum. Legacy `store` still
requires an integer. Uninitialized/invalid cells fail; allocation exhaustion is
a resource error. No coercion, implicit copy of mutable identity, or rollback.

## Recursive actions

`unfolding A` installs a recursive delimiter D(A,n), executes A, and propagates
its outcome. `unfold` substitutes the nearest D's body at the current action,
using its defining syntax node but current bindings/transients/store, preserving
all pending combinators. `tailunfold` discards pending frames above D before
restarting. Thus `(unfold and then give 99)` preserves 99; tailunfold deliberately
does not. No enclosing delimiter, or crossing a next delimiter, causes failure.
Recursion uses explicit frames and is resource bounded; only explicit tail
operations discard observable pending work.

## Branches and loops

`if e then A else B` executes exactly A if eval(e) is a nonzero integer, otherwise
B; a non-integer fails. Failure inside A never selects B.

`while e do A`: evaluate e under original ρ,d and current σ; zero completes
empty, nonzero executes A. Completion or continue repeats; failure/escape
propagates. Body output tuples/bindings are discarded, while store writes persist.
Break completes the loop empty. Break/continue discard frames up to the nearest
while in the current function; absence of such a loop fails. Function calls cannot
break their caller's loop. Return bypasses the loop to its function boundary.

## Continuations

```text
callcc f:
  k = OneShot(copy(K)); apply f to [k] under current store
resume k with e:
  if k.used then fail
  else v=eval(e); k.used=true; K=copy(k.frames); deliver done([v],{})
```

Capture excludes the callcc action; normal return from f is ordinary completion.
Used is shared identity, not part of the restored control copy. Store, cell
allocation counters, step counters and continuation-used flags are never rewound.
Saved frame slots retain environments, syntax nodes, pending results and handler
states, including legacy trapping/otherwise. There is no multi-shot cloning.

## Generators

`generator A` creates G(suspended,[initial frame(A,n,ρ,d)]) and completes with G.
`next G` installs N(G,K) and restores the suspended frames. `yield_value e`
evaluates e, saves frames above the nearest N excluding itself, and returns
`done([1,eval(e)],{})` outside N. On resumption, that yield completes empty.
Normal body completion returns `done([0] ++ d,b)` and closes G. Failure or escape
closes G and propagates its outcome. Next on closed G completes `[0]`.

`close G` changes suspended to closed, discards frames, completes empty; repeated
close succeeds. Running next/close fails (no reentrancy). Yield without N fails.
Generators may nest, recurse and retain shared cells, but cannot be combined with
callcc/resume in one synthesized specification yet.

## Errors and limits

Dynamic tag mismatch, missing bindings/arguments, repeated resume and invalid
control scope are semantic failure. Declared limits are terminal resource errors;
neither fallback nor trapping turns them into success. Unsupported syntax is a
frontend diagnostic; the unsupported cross-delimiter combination is a synthesis
capability diagnostic. Cleanup/finally/cancellation/host effects are not defined
or accepted, so no cleanup guarantees are implied by close or control transfer.
