# Compile-time templates and hygienic semantic macros

LLSynth supports two bounded, backend-independent forms of metaprogramming.
They expand parsed action ASTs before name/type checking and emission, not
source text. Generated Python 3 and native C11 use the same expanded action IR;
no macro runtime or new runtime instructions are required.

## Expression templates

```text
.template Twice(x) = x + x
.template Offset(x, amount) = Twice(x) + amount

.rule Start
'syntax: value:NUMBER EOF
'semantics: return Offset($value, 1);
..
```

A `.template Name(parameters) = expression` declaration occupies one line.
The body is exactly one existing semantic expression (without a semicolon).
Parameters are untyped identifier names and arguments are expression ASTs.
Expansion substitutes ASTs, preserving grouping and caller bindings:
`Twice(2 + 3)` means `(2 + 3) + (2 + 3)`, not `2 + 3 + 2 + 3`
through a textual replacement mechanism.

Expansion occurs at generator time; the resulting expression still executes
at program runtime. This is not arbitrary Perl execution or a constant-evaluation
facility. A parameter appearing twice duplicates evaluation of its argument;
an unused parameter does not evaluate its argument. Use a macro or an ordinary
function when arguments must evaluate exactly once.

Templates work in functions, `.init`, all semantic action facets
(`semantics`, `lower`, `analyze`, `emit`), state initializers, rewrite guards,
and rewrite replacement operands. Rewrite purity/type/effect checks still
apply after expansion. Templates do not expand grammar syntax, types,
declaration names, rewrite match operands, token regexes, or strings.

## Hygienic statement macros

```text
.state @total: Int = 0

.macro Accumulate(value)
let tmp = value;
@total = @total + tmp + tmp;
.end

.rule Start
'syntax: value:NUMBER EOF
'semantics:
    let tmp = $value;
    Accumulate(tmp + 1);
    return tmp;
..
```

`.macro Name(parameters)` introduces an ordinary action statement list,
terminated by `.end`. Invoke it as a whole expression statement:
`Accumulate(expression);`. The equivalent `call Accumulate(expression);`
and `discard Accumulate(expression);` forms also work because they parse to
the same statement AST. A macro cannot appear inside an expression.

Each argument is evaluated **once, left-to-right**, even if unused. Its value
is stored in a fresh local before the expanded body. Mutating a parameter
rebinds this local, not the caller's variable; referenced vectors/tables retain
ordinary shared-value behavior. `return` in a macro returns from the surrounding
action or function; it does not introduce a function boundary.

### Binding and hygiene

Expansion resolves binders and references structurally:

* Every macro parameter, `let`, `grab` result, and `for` binder gets a fresh,
  deterministic name. Nested macro calls have independent bindings.
* Caller argument ASTs are expanded in the caller environment **before**
  entering the macro's environment. A macro-local `tmp` cannot capture the
  caller's `tmp`, and the caller cannot capture a macro-local name.
* When metaprograms are present, caller local binders and function parameters
  are also alpha-renamed. This prevents caller locals named `len`, `print`,
  etc. from capturing definition-site builtin references. Generated names
  are checked against all existing AST names, including user-chosen names.
* Definition bodies can refer to declared functions, builtins, explicit
  `@state`, explicit `#tables`, and qualified IR operations. Bare declared
  functions are resolved as explicit `&function` references, immune to
  caller shadowing.
* Free caller locals and `$captures` in definition bodies are errors, not
  implicit capture. Pass them explicitly as parameters. An `on` branch in a
  macro definition is rejected for the same reason; invoke the macro inside
  the caller's `on` branch instead.
* Strings are never interpolated by this facility.

Macros cannot export locals into the caller. Use explicit state effects,
mutable values, a returning expression template, or an ordinary function.
Local duplicate definitions in a block and parameter/local duplicates are
rejected; nested blocks may shadow bindings with distinct fresh names.

## Declaration and expansion rules

Definitions may be included with `.include`; forward references are allowed.
Templates and macros share a compile-time namespace. Duplicate names, duplicate
parameters, runtime function/builtin/IR collisions, and use of a compile-time
name as a local binder or parameter are rejected. Arity is exact; defaults,
variadics, overloads, and typed template parameters are not supported.

All definitions are structurally/binding checked, including unused ones;
expanded instances undergo the usual action type and effect checks.
Direct/indirect expansion
recursion is an error with an expansion trace; ordinary runtime function
recursion is unaffected. Expansion is bounded by 64 nested calls, 4096
invocations, and 100000 AST visits/copies per validation or expansion context.
Resource exhaustion is an error, never partial successful generation.
Fresh-name allocation and traversal order are deterministic.

Diagnostics identify the definition location for invalid definitions, or the
containing action location for invalid invocations. Arity diagnostics also
include the definition location. Action diagnostics currently report the
containing rule/function's declaration line, not individual body-token spans.

This implementation supports the existing expression AST and action statements:
`let`, `grab`, calls, `discard`, `return`, assignments, `require`, `Throw`,
`while`, `for`, `if`/`else`, caller-side `on` branches, lexical closures, and
`await`. Closure parameters and body binders are renamed structurally, retaining
lexical captures without capturing caller arguments. Async/task builtins are
definition-site references, too. Closures and async execution still require
the Python tool target; expansion does not remove that restriction. Other new
action AST forms need explicit binder-aware expansion support; unsupported
forms are diagnosed instead of being treated as unhygienic text.

Modules can export templates and macros using `.export Name`. Call them as
`Alias__Name(...)`; module linking resolves declaration-site functions, state,
and other metaprograms before AST expansion. See [modules](modules.md).

## Existing backend templates

`.backend`, `.emit`, `.prologue`, `.epilogue`, and `${operand}` interpolation
retain their previous meaning. They are target code-generation text templates,
not semantic macros, and are never rewritten by semantic metaprogramming.
No existing specification needs to opt into the new declarations.

When relocating the generator, keep `LLSynthMeta.pm` beside `llsynth.pl`,
`LLSynthCompiler.pm`, and `LLSynthEmit.pm`. The driver tracks the expander
module as a generator input, including generated-file dependency metadata.
Keep `LLSynthModules.pm` beside the driver as well.

## Tests

From the repository root:

```sh
python3 cornerstone/LLSynth/tests/metaprogramming/test_metaprogramming.py
```

The suite exercises definition/caller binding, nested scope and macro hygiene,
exactly-once arguments, expression composition, state initializers, lowering,
backend-template compatibility, deterministic generation, and JSON diagnostics.
Native parity tests compile generated C with `-std=c11 -pedantic-errors` when
a C compiler is available (`CC` overrides the compiler).
