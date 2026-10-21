# 3. Textual language

`frontend/parser.cpp` is the bounded recursive-descent implementation; there is
no separate grammar file or generated parser to hand-edit. Parsing constructs the owning IL directly. Parsing and semantic
verification are separate operations.

## Lexical rules

- Identifiers use ASCII `[A-Za-z_][A-Za-z_0-9.-]*`.
- Value names start with `%`; block names with `^`; function/symbol names with `@`.
- `//` comments continue to the end of the line.
- Integers are signed decimal int64 values; unsigned bit patterns are written
  using their signed representation. Hexadecimal literals are not supported.
- Reals contain a decimal fraction and/or exponent, for example `1.0`, `-0.5`,
  `2e3`. Finite binary64 literals are supported.
- Strings use double quotes and `\\`, `\"`, `\n`, `\r`, `\t` escapes. Raw
  control bytes and unsupported escapes are errors. UTF-8 bytes can occur in
  strings, not identifiers.

Numeric IDs can be used directly. Avoid mixing numeric IDs with symbolic IDs
that happen to receive the same number: collisions are rejected rather than
silently aliasing two values. Value and block names have separate namespaces.
Forward references are resolved before dominance checking; this is necessary
for loop phis, but does not authorize ordinary use-before-definition.

## Module declarations

```text
module example level high {
  target "vm" {
    pointer_bits = 64;
    index_bits = 32;
    endian = "little";
    feature.example = true;
    policy = "application-defined";
  }
  types { T = i32; U = tuple<param<T>, ptr<generic>>; }
  dialect math { factor = 3; }
  attrs { producer = "my frontend"; schema = 1; }
  import "std";
  // Functions follow.
}
```

An `import "name";` declaration names a standard library module. Imports are
resolved explicitly before verification; see
[the standard library chapter](10-stdlib-and-imports.md). Library names are
plain identifiers, never paths, and duplicates are parse errors.

The default target is the explicitly defined portable VM layout: 64-bit
pointers/indices and little-endian byte order. This is not host detection.
Target keys other than `pointer_bits`, `index_bits`, `endian` and `feature.*`
are application-defined target parameters. Feature declarations must be `true`.
Duplicate target declarations, attributes, bindings or dialect instances are
parse errors.

An attribute is an int64, finite double, Boolean, string, or `type<T>` value.
Its variant is significant: `1`, `1.0` and `true` are different values. Parameter
overrides must have the same variant as their registered defaults.

## Types

| Spelling | Meaning |
| --- | --- |
| `void` | No function result; never a value. |
| `iN` | N-bit integer, including Boolean `i1`. |
| `fN` | Floating type; builtin executable literals require exactly `f64` (so `1.0:f32` is rejected). |
| `index` | Integer specialized to the target's index width. |
| `ptr<space>` | Address in a named address space. |
| `vector<count, element>` | Positive fixed-length vector with a scalar element. |
| `tuple<T, U, ...>` | Ordered product type. |
| `opaque<dialect.name, T, ...>` | Dialect-verified opaque/parameterized type. |
| `param<T>` | Named type parameter resolved from the module's bindings. |

An IL can represent integer widths through 65536 bits; the scalar VM supports
only 1–64 bits. Representability is not a promise of a particular backend's
capability. Aggregates and opaque types require explicit scalarization or a
native target adapter.

## Functions and blocks

```text
extern func @host(%x:i32) -> i32;

func @choose(%condition:i1, %x:i32, %y:i32) -> i32 {
  entry ^entry;
  ^entry "entry block":
    cf.branch(%condition) to [^yes, ^no];
  ^yes:
    cf.jump() to [^join];
  ^no:
    cf.jump() to [^join];
  ^join:
    %r:i32 = cf.phi(%x, %y) from [^yes, ^no];
    func.return(%r);
}
```

If no `entry` declaration appears, the first declared block is the entry.
Canonical output always prints it explicitly. A block's optional quoted label
is descriptive metadata, independent of its identity.

Entry blocks cannot have incoming edges. Every defined function has a body,
and every block ends in exactly one terminator. The current CFG supports up to
two explicit successor blocks. Unreachable blocks are rejected; frontends must
remove them or connect them explicitly. External declarations have parameters
and a result but no body/entry block.

All function parameters dominate all blocks. Ordinary SSA definitions must
dominate their uses and precede uses in the same block. A phi's operand is used
on its labelled incoming edge, not at the phi's textual position. Phis precede
other operations and cover every predecessor exactly once.

## Generic operation syntax

```text
%result:i32 = arith.add(%x, 2:i32);
mem.store(%address, %result);
%a:i32, %b:i32 = custom.split(%result);
%called:i32 = func.call(%result) attrs { callee = "host"; };
cf.branch(%predicate) to [^then, ^else];
%joined:i32 = cf.phi(%a, %b) from [^left, ^right];
```

Result declarations are omitted for effect/terminator operations. Operands are
value references, typed integer/real literals, or typed symbols such as
`@global:i32`. Operation-specific verifiers decide whether a symbol is legal;
symbols are not automatically resolved as local SSA values.

The optional suffix order is `to`, then `from`, then `attrs`. Each operation
ends in a semicolon. The parser accepts registered and unregistered names alike;
verification rejects names absent from the registry.

## Builtin operations

- `arith.constant`, `arith.copy` — one operand/result. Constants require a
  numeric literal; copies may reference an existing value.
- `arith.add`, `sub`, `mul`, `div`, `mod`, `and`, `or`, `xor`, `shl`, `shr` —
  two same-typed operands, one result of that type.
- `arith.neg`, `not` — one operand/result.
- `arith.eq`, `ne`, `lt`, `le`, `gt`, `ge` — same-typed scalar operands, `i1`
  result. Ordered integer comparisons are signed.
- `arith.select` — `i1` condition and two matching alternatives.
- `cf.phi` — one or more operands plus predecessor labels.
- `cf.jump` — no operands, one successor.
- `cf.branch` — one `i1` operand, two successors, true successor first.
- `func.call` — signature-checked operands/results and exactly one string
  `callee` attribute.
- `func.return` — zero operands for void, otherwise the declared return type.
- `mem.load` — `ptr<generic>` address operand, one typed result. The loaded value
  type is not constrained to the address; the VM heap holds dynamically typed
  values and only the generic heap-cell address space is executable.
- `mem.store` — `ptr<generic>` address and stored value, no result. The stored
  value type is similarly unconstrained at verification time.
- `math.square`, `math.madd`, `math.scale`, `math.cube`, `math.abs` — high-level
  integer operations over `i2`–`i64`. `madd(a,b,c)` means modular multiply
  followed by modular add, not an implicit floating FMA. `scale` uses the
  dialect instance's `factor` parameter. `abs(x)` selects between `-x` and `x`;
  `neg(MIN)` wraps to `MIN` under modular semantics.
- `vector.splat` — one scalar operand, one `vector<N,T>` result.
- `vector.add`, `sub`, `mul`, `div`, `mod`, `neg` — same-typed vector operands.
  `mod` requires integers, like its scalar counterpart.
- `vector.extract` — one vector operand plus `attrs {lane = i;}` with
  `0 <= i < N`, one scalar result of the element type.
- `tuple.build` — one or more scalar operands, one tuple result whose fields
  match the operand types in order.
- `tuple.extract` — one tuple operand plus `attrs {index = i;}`, one result of
  the field type.
- `ptr.add` — pointer base and integer offset, one pointer result in the same
  address space. Native-only: the portable VM has no address arithmetic, so
  execution rejects it explicitly.
- `atomic.load`, `atomic.store` — like `mem.load`/`mem.store` with
  `attrs {ordering = "...";}`. Loads accept `relaxed`, `acquire`, `seq_cst`;
  stores accept `relaxed`, `release`, `seq_cst`. Native-only, like `ptr.add`.
- `cf.trap` — no operands, results, attributes or successors. A high-level
  terminator that lowers to a throwing runtime call; execution fails with
  `vm.trap`.

Integer-only operations include bitwise operators, shifts, remainder and `not`.
For `i1`, only comparisons, Boolean bitwise operators, copies, phis and selection
are supported—not addition/multiplication. Builtin core operations reject extra
attributes rather than ignoring potentially meaningful flags.

The `vm` dialect mirrors scalar core operations using names such as `vm.add`,
`vm.phi`, `vm.call`, and `vm.return` at machine level. It retains the same
semantics; bytecode compilation performs the actual register/stack assignment.

Next: [Types and dialect extensions](04-dialects-and-types.md).
