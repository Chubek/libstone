# GateML 0.1

A strict, immutable ML-style language implemented in C++20 using
`third_party/metatk/MetaChain`. This is **not** the old GateML frontend-definition
DSL, nor a complete OCaml or Standard ML implementation.

## Build and run

```sh
cmake -S gatestone/gateml -B build-gateml
cmake --build build-gateml --parallel
ctest --test-dir build-gateml --output-on-failure
build-gateml/gateml-cli run -e 'List.sum (List.map succ [10; 20; 9])'
# 42
build-gateml/gateml-cli type -e 'fun x -> x'
# ('a -> 'a)
```

CMake 3.20+, C++20, and a populated vendored MetaTk are required. Python 3 is
required only for tests. There are no downloads or parser-generation steps.
The Gatestone parent and aggregate Libstone builds also work; the aggregate
option is `LIBSTONE_BUILD_GATESTONE=ON`. Dependencies honor
`LIBSTONE_THIRD_PARTY_DIR` and `LIBSTONE_METATK_SOURCE_DIR`.
`GATEML_BUILD_TESTS` and `GATEML_BUILD_TOOLS` control tests and CLI installation.

## Language

```gml
let compose f g x = f (g x);;
let rec factorial n = if n = 0 then 1 else n * factorial (n - 1);;
let identity x = x;;
let answer = let x = 40 in x + 2;;
(identity answer, identity true)
```

* Programs contain zero or more `let ... ;;` declarations, followed by an
  optional result expression. Empty programs and declaration-only programs
  return `()`. Local bindings use `let ... in ...`.
* Functions use `fun x y -> expression`, or `let f x y = expression` sugar.
  Application is left associative and binds tighter than binary operations.
  Functions are curried, first class, and lexically scoped. `let rec` requires
  a function and supports direct recursion, including curried functions.
* Evaluation is left-to-right and call-by-value. `&&` and `||` short circuit.
  `a; b` evaluates both and requires `a : unit`.
* Hindley–Milner inference provides let polymorphism, monomorphic recursion,
  and an occurs check. There are no implicit conversions or truthy values.
* Types: signed 64-bit `int`, `bool`, byte `string`, `unit`, homogeneous
  `'a list`, pairs, `'a option`, `('a, 'e) result`, and function arrows.
* Values include `true`, `false`, `()`, `[1; 2]`, `h :: t`, `(a, b)`, `None`,
  `Some x`, `Ok x`, and `Error e`. Constructors are ordinary typed functions
  and may be partially applied, passed, or shadowed in expressions.
* Integers are decimal, with optional unary minus. Overflow wraps modulo
  2^64. Division truncates toward zero; remainder follows the dividend's sign.
  Division/remainder by zero and `min_int / -1` (also remainder) trap.
  `Int.abs min_int` remains `min_int`. Shifts require 0–63; right shift is
  **logical**, matching MetaIL.
* Strings support `\n`, `\r`, `\t`, `\\`, and `\"` escapes. Lengths and indexes
  count bytes, not Unicode characters; `String.get` returns a one-byte string.
* `(* comments (* nest *) *)` and `// line comments` are accepted.
  Locations count source bytes, not Unicode code points.

### Operators, low to high precedence

| Operators | Meaning |
|---|---|
| `;` | sequence (right associative) |
| `||` | short-circuit disjunction |
| `&&` | short-circuit conjunction |
| `= <> < > <= >=` | equality and integer ordering |
| `::` | list construction (right associative) |
| `^` | string concatenation |
| `+ -` | integer addition/subtraction |
| `* / mod` | integer multiplication/division/remainder |
| unary `-`, `not` | negation |
| application | function call |

Other binary operations are left associative. Structural equality works on
data values; encountering a function during comparison traps (including inside
a container). Equality does not require the same physical object. List element
separators are not sequencing: write `[(print_int 1; 2); 3]` when needed.

Arithmetic, comparison, boolean, and string-concatenation operators can also
be passed as parenthesized function values:

```gml
let total = List.fold_left (+) 0 [1; 2; 3];;
let subtract_from_ten = (-) 10;;
(total, List.map subtract_from_ten [1; 2; 3])
```

`(-)` is binary subtraction, not unary negation; use `-x` or `Int.neg x` to
negate. `(not)` is unary, and `(mod)` names remainder. `(*)` and `( * )` both
name multiplication; outside comments, exact `(*)` is an operator value rather
than a comment opener. Ordinary and nested `(* ... *)` comments are unchanged.
Partial application binds the **first** argument: `((>) 2) x` means `2 > x`.
Unlike infix `&&`/`||`, the function values `(&&)`/`(||)` evaluate both arguments
strictly. Operators cannot be rebound with a symbolic `let` name in this version.

### Pattern matching

```gml
let rec sum xs = match xs with
  | [] -> 0
  | head :: tail -> head + sum tail
;;
match (Some 4, [2; 3]) with
  | (Some x, h :: t) -> x + h
  | _ -> 0
```

Patterns support variables, `_`, integer/boolean/unit constants, `[]`, `::`,
pairs, `None`, `Some`, `Ok`, and `Error`. Parenthesize nested constructor
patterns when needed. Duplicate bindings in a pattern are errors. Branches are
tried in source order and must return the same type. Non-exhaustive matches
trap at runtime; exhaustiveness and redundancy are not statically checked.
String patterns, guards, and or-patterns are not implemented.

### Namespaces

```gml
module Math = struct
  let base = 40;;
  let add x = base + x;;
end;;
open Math;;
add 2
```

Modules contain declarations only, may nest, and export qualified names
(`Math.add`). `open Math;;` exposes its names in the following lexical program;
local bindings still shadow them. Standard-library namespaces can be opened
(`open List;;`). These are transparent, statically flattened namespaces, **not**
first-class module values. Nested exports can be used by their qualified names;
opening a nested module by its full path is not supported in this version.

There are no signatures, functors, abstract/user-defined types, mutable cells,
arrays, floats, objects, exceptions/handlers, mutual recursion (`and`), type
annotations, destructuring function parameters/let bindings, file imports,
serialization, or external/dynamic FFI. Unused `.cpp`/header scaffolding in this
directory does not imply those facilities exist. Invalid/unsupported syntax
is diagnosed, not silently ignored.

## Standard library

The core implemented library is embedded at configure/build time, with
source under `stdlib/`. `stdlib.gml` documents its assembly order. Each file is
ordinary GateML and is typechecked with the program. No installed source-tree
lookup is required. `--no-stdlib` disables source-library loading; the small
typed primitive set remains available.

The other pre-existing empty stdlib directories are **not implemented**. See
[the explicit implementation-status list](stdlib/README.md); this release does
not complete every planned facility (networking, IPC, binary formats, etc.).

| Namespace | Exports |
|---|---|
| Prelude | `id const flip compose ignore succ pred min max` |
| Int | `min_int max_int abs neg compare clamp gcd pow to_string of_string logand logor logxor shift_left shift_right` |
| Bool | `not to_int to_string equal` |
| Pair | `fst snd swap map` |
| Option | `is_some is_none value map bind get` |
| Result | `is_ok is_error value map map_error bind to_option` |
| String | `length get sub compare of_int is_empty equal starts_with ends_with` |
| List | `is_empty length rev head tail map filter fold_left fold_right append concat concat_map sum product exists for_all mem find nth take drop range iter` |
| IO | `print_int println_int print_string print_endline print_bool` |

`fst`, `snd`, `Some`, `None`, `Ok`, `Error`, `failwith`, and the four output
primitives are also global. Important argument conventions:

* `Option.value default option`, `Result.value default result`.
* `Option.bind option f`, `Result.bind result f`.
* `Pair.map f g pair`, `Int.clamp low high value`.
* `List.fold_left f initial xs`, `List.fold_right f xs initial`.
* `List.range low high` is inclusive (empty if low > high).
* `List.head`, `List.tail`, `List.nth`, `List.find` return options.
  Negative `nth` returns `None`; nonpositive `take`/`drop` return `[]`/the input.
* `Int.of_string` returns `None` for malformed/out-of-range input. It accepts
  only whole decimal strings with an optional minus, not spaces or plus signs.
* `String.sub string start length`; invalid indexes trap.
* `print_int` has no newline; `println_int` and `print_endline` append one.
  Output is captured, not sent directly to the host terminal by the VM.

`Option.get None`, `failwith`, and negative `Int.pow` exponents trap. Some list
functions (e.g. `map`, `fold_right`) intentionally retain non-tail recursion;
the VM bounds their work. Tail calls in the bytecode backend reuse call frames.

## CLI and native compilation

Commands: `run`, `check`, `type`, `ir`, `build`, and `stdlib`.
Inputs are files or `-e SOURCE`; `run` prints captured output followed by the
displayed result. `check` typechecks and lowers without executing. `ir` prints
GateML closure bytecode, not integer MetaIL. `--max-steps N` bounds execution;
`--help` and `--version` are available. Exit codes: 0 success, 1 compilation/
runtime/I/O error, 2 usage error. Diagnostics include file, line, byte-column.

```sh
build-gateml/gateml-cli build gatestone/gateml/examples/factorial.gml -o factorial
chmod +x factorial
./factorial
```

The native path genuinely lowers to verified MetaIL, optionally optimizes it,
then uses MetaChain's native lowering, ASMtk, and LDtk. It does not shell out.
It emits a static, non-PIE Linux x86-64 ELF executable. Native support is limited
to int/bool/unit values, scalar lets, scalar operators/conditionals/sequencing,
fully applied statically named first-order functions (including recursion and
lexically captured scalar locals), and `println_int`/`IO.println_int`. Scalar
standard-library functions can be used when their bodies fit this subset.
Partial/higher-order calls, immediate lambda calls, data containers, patterns,
strings, other primitives, and function-valued results are rejected.
Parenthesized operator values currently use the bytecode backend only; use
infix operators in native scalar programs.
There are 30 available scalar-local registers per lexical context. Native tail
calls are ordinary calls. Inferred types are checked before dead-code folding.

Native results are process exit statuses (low 8 bits), not printed values.
MetaChain native traps use exit codes 124 (step budget), 125 (stack budget),
126 (arithmetic), and 127 (output error). Native and bytecode instruction budgets
count different instruction sets. Native output writes directly to stdout and
does not enforce the bytecode output-byte budget. No JIT is provided.

## Embedding and installation

```cpp
#include <GateML.h>
auto program = gateml::compile("let f x = x + 1 in f 41");
auto result = gateml::run(program);
// program.type == "int", result.value.integer() == 42
```

Link `Gatestone::GateML`. Programs are reusable immutable handles; separate
`run` calls have independent state. `Value` is owned and supports `kind`,
`integer`, `boolean`, `string`, `fields`, and bounded `show`. Functions are
inspectable as values but cannot be invoked directly from C++ in this version.
`Execution` contains `value`, captured `output`, and `steps`.
`gateml::Error` carries a `Location` and message. `standard_library()` exposes
the embedded source; `disassemble()` exposes bytecode.

```sh
cmake --install build-gateml --prefix /some/prefix
```

Installed CMake consumers use `find_package(GateML CONFIG REQUIRED)` and link
`Gatestone::GateML`. The public header does not expose MetaChain or require its
headers in an installed consumer. The library, CLI, and library sources install
under the standard CMake GNU install directories.

## Architecture and limits

* ParseTk performs committed, located lexical parsing using a fixed PEG grammar.
  The GateML precedence/pattern parser builds an owned syntax tree from events.
* Semantic analysis implements HM unification/generalization. Local constant
  folding reuses MetaIL integer semantics without evaluating effects or traps.
* Closure conversion captures only free values. Bytecode runs in a custom ISA
  on **MetaChain MetaVM**, not a separate AST evaluator. Immutable data and flat
  closure environments use shared ownership; recursive functions bind their
  own name on invocation rather than creating ownership cycles. Destruction of
  long data/closure chains is iterative.
* Native scalar lowering uses MetaIL's builder/verifier/optimizer and the
  collective `metachain::Toolchain` assembly/linking API.

`Options` bounds source bytes, syntax/typechecking work, structural depth,
instructions/produced values, VM steps, produced string bytes, and captured
output. Value-allocation accounting is conservative and cumulative (per VM
instruction, not a garbage-collected live-heap measurement). String budgets
count intermediate string results conservatively. Display has separate depth/
byte ceilings. These are engineering guards, **not a hardened security sandbox**
or exact OS memory limits. Increasing recursion/type limits can exhaust the
host C++ stack. Native processes need OS-level limits for untrusted code.

Tests cover type errors, polymorphism, lexical scope, recursive/curried and tail
calls, modules, pattern branches, library functions, output/resource errors,
reusable handles, CLI contracts, and actual native execution on Linux x86-64.
