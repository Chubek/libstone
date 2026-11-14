# Sequent IR: design and relation to the paper

The paper *Sequent Calculus as a Compiler Intermediate Language* (ICFP 2016)
models computation as the interaction of a **term** and a **continuation**.
This implementation uses separate syntax nodes for `Term`, `Frame`, `Kont`
and `Command`. Its fundamental cut is `Cut(term, kont)`. A finite continuation
is a stack of applications (value or type) ending in `ret` or `case`.

A result-producing command can be reified as the term `Mu(type, command)`,
which creates a **new local `ret`**. The defining rewrite is:

```
  <mu ret. c || k>  ==>  c[k/ret]
```

The `append_continuation(c,k)` operation implements the structurally scoped
replacement: terminal return frames are extended by `k`, case alternatives
are visited, and join definitions share the same outer ret. It never enters
nested `Mu` terms and therefore does not capture nested return destinations.

Join labels are *commands*, not ordinary function values. A declaration
`LetJoin(j, [x:T], body, continuation)` defines a lexically scoped destination
`jump j v`. This makes sharing explicit, and keeps joins stable when a
continuation is substituted through a computation.

## Core syntax

```
Term       = Var | Int | Float | Bool | String | Unit | Lambda
           | TypeLambda | Constructor | Mu(Command)
Frame      = Apply(Term) | TypeApply(Type) | Case(Alternatives)
Kont       = Frame* ; final ret is implicit; case must be terminal
Command    = Cut(Term, Kont) | Jump(Label, Terms, TypeArgs)
           | LetValue(Name,Type,Term,Command)
           | LetRec(Names,Types,Terms,Command)
           | LetJoin(Label,TypeParams,Params,Command,Command)
```

Labels and values are different name spaces. A `Mu` term is checked with an
empty label environment, enforcing the paper's restriction that terms cannot
capture labels from surrounding commands. This differs from unrestricted
classical-control calculi and is intentional to preserve a pure interpretation.

## Verifier requirements

- All variables, labels, type variables and constructor names must be bound.
- Cut producer type must match consumer input type.
- App requires Arrow, TypeApply requires Forall, and their types must agree.
- All case alternatives must be type-consistent and exhaustive for Booleans and
  registered data constructors; a typed default alternative is also allowed.
- Joins have exact argument and type-argument arities; self-recursion is legal.
- A `Mu` has a unique return type; any jumps inside it must be local.
- Rec value groups are mutually recursive, but value *terms* may not refer
  to surrounding continuation labels.

Note: `Type::Var` types are in scope only under explicit `forall`/type-lambda or
join type parameters. The verifier is not an inference engine.
