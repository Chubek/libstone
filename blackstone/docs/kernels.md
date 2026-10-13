# Executable kernel inventory

The catalog contains the original `strength_reduction` kernel and the following
70 additions. Each identifier is independently loadable, queryable and executable;
each file declares its own requirements, preservation contract and documentation.
All additions target `ssa`. Their shared implementation is `lrewrite/local.lua`.

## Common contract

- Explicit modular semantics for built-in, unnamed integer types, width 1–64.
- Valid SSA, typed operands, definitions and parameter types are required.
- `def_use` is required by every addition; CSE also requires `dominators`.
- All additions preserve `cfg` and `dominators`. Def-use and loop/induction
  facts are conservatively invalidated when the kernel reports a change.
- Literal constants may be supplied by constant/copy definitions, except
  `phi_constant`, which requires literal incoming operands.
- Unknown facts, unknown arithmetic semantics, floats and custom-named types
  do not authorize rewrites. Reflexive comparisons must have integer operands,
  not merely Boolean results; NaN-sensitive floating-point identities are excluded.
- Keep existing ValueIds and operand-producing instructions. A replaced result
  becomes a copy or constant; dead definitions are retained. Effects or traps in
  a producing load, call or arithmetic computation are not removed.
- Each invocation traverses a deterministic snapshot once. Apply another kernel
  or invoke again to exploit opportunities exposed by a prior transformation;
  fixed-point scheduling belongs to the host. Profitability is target-dependent.

In the tables, `a` and `b` are proven same-width constants; `x`, `y` and `z`
denote integer SSA operands; `c` is a Boolean condition; `ones` is the width-bounded
all-ones bit pattern. Repeated symbols require the same ValueId or equal literal
bit patterns. Commuted patterns are accepted only where explicitly indicated.

## Constant folding — 18

Files: `kernels/local/constant-folding/<opcode>.lua`.

| Kernel | Transformation | Additional restriction |
| --- | --- | --- |
| `fold_add` | `a+b -> constant` | Modular wraparound |
| `fold_sub` | `a-b -> constant` | Modular wraparound |
| `fold_mul` | `a*b -> constant` | Modular wraparound |
| `fold_div` | `a/b -> constant` | Width >=2; both nonnegative and signed-representable; `b>0` |
| `fold_mod` | `a%b -> constant` | Same common signed/unsigned domain and positive divisor |
| `fold_and` | `a & b -> constant` | Width-bounded bitwise evaluation |
| `fold_or` | `a \| b -> constant` | Width-bounded bitwise evaluation |
| `fold_xor` | `a ^ b -> constant` | Width-bounded bitwise evaluation |
| `fold_shl` | `a << b -> constant` | Width >=2; `0<=b<width`; discard shifted-out bits |
| `fold_shr` | `a >> b -> constant` | Width >=2; clear input sign bit; `0<=b<width` |
| `fold_neg` | `-a -> constant` | Modular negation, including the minimum bit pattern |
| `fold_not` | `~a -> constant` | Bitwise complement; Boolean NOT at width one |
| `fold_eq` | `a==b -> Boolean constant` | Compare normalized bit patterns |
| `fold_ne` | `a!=b -> Boolean constant` | Compare normalized bit patterns |
| `fold_lt` | `a<b -> Boolean constant` | Both constants have clear sign bits |
| `fold_le` | `a<=b -> Boolean constant` | Both constants have clear sign bits |
| `fold_gt` | `a>b -> Boolean constant` | Both constants have clear sign bits |
| `fold_ge` | `a>=b -> Boolean constant` | Both constants have clear sign bits |

Division and remainder never fold zero divisors, negative/high-bit inputs or
`MIN/-1`. Signedness is not encoded in Firestone's types. Consequently, ordered
comparisons and right shifts use domains where signed and unsigned behavior agree.

## Algebraic simplification — 29

Files: `kernels/local/algebraic/<hyphenated-kernel-name>.lua`.

| Kernel | Transformation | Matching |
| --- | --- | --- |
| `add_zero` | `x+0 -> x` | Either operand order |
| `sub_zero` | `x-0 -> x` | Right zero only |
| `mul_one` | `x*1 -> x` | Either operand order |
| `mul_zero` | `x*0 -> 0` | Either operand order |
| `div_one` | `x/1 -> x` | Width >=2; positive one divisor |
| `mod_one` | `x%1 -> 0` | Width >=2; positive one divisor |
| `and_zero` | `x & 0 -> 0` | Either operand order |
| `and_all_ones` | `x & ones -> x` | Either operand order |
| `or_zero` | `x \| 0 -> x` | Either operand order |
| `or_all_ones` | `x \| ones -> ones` | Either operand order |
| `xor_zero` | `x ^ 0 -> x` | Either operand order |
| `xor_self` | `x ^ x -> 0` | Identical operands |
| `sub_self` | `x-x -> 0` | Identical operands |
| `and_self` | `x & x -> x` | Identical operands |
| `or_self` | `x \| x -> x` | Identical operands |
| `shl_zero` | `x << 0 -> x` | Proven zero count |
| `shr_zero` | `x >> 0 -> x` | Both logical and arithmetic shift |
| `double_negation` | `-(-x) -> x` | Inner SSA negation |
| `double_not` | `~(~x) -> x` | Inner SSA complement |
| `add_negation` | `x+(-x) -> 0` | Either outer operand order |
| `subtract_negation` | `x-(-y) -> x+y` | Inner SSA negation |
| `and_absorption` | `x & (x \| y) -> x` | Either order at both nodes |
| `or_absorption` | `x \| (x & y) -> x` | Either order at both nodes |
| `xor_cancel` | `(x ^ y) ^ y -> x` | Either order at both nodes |
| `add_sub_cancel` | `(x+y)-y -> x` | Either order in the inner add |
| `sub_add_cancel` | `(x-y)+y -> x` | Either order in the outer add |
| `sub_sub_cancel` | `x-(x-y) -> y` | Ordered subtraction operands |
| `and_complement` | `x & ~x -> 0` | Either outer operand order |
| `or_complement` | `x \| ~x -> ones` | Either outer operand order |

Reassociation and cancellation require modular arithmetic rather than trapping
overflow. The kernel does not delete the inner negation, complement or other
producer, preserving the original evaluation of those definitions.

## Reflexive comparisons — 6

Files: `kernels/local/comparisons/<opcode>-self.lua`.

| Kernel | Transformation |
| --- | --- |
| `eq_self` | `x==x -> true` |
| `ne_self` | `x!=x -> false` |
| `lt_self` | `x<x -> false` |
| `le_self` | `x<=x -> true` |
| `gt_self` | `x>x -> false` |
| `ge_self` | `x>=x -> true` |

These rules agree for signed and unsigned integer operands. None is generalized
to floating-point values; the producing operands remain evaluated.

## Select simplification — 5

Files: `kernels/local/select/<hyphenated-kernel-name>.lua`.

| Kernel | Transformation | Restriction |
| --- | --- | --- |
| `select_true` | `select(true,x,y) -> x` | Proven Boolean condition |
| `select_false` | `select(false,x,y) -> y` | Proven Boolean condition |
| `select_identical` | `select(c,x,x) -> x` | Identical arms |
| `select_boolean_identity` | `select(c,true,false) -> c` | One-bit result |
| `select_boolean_inversion` | `select(c,false,true) -> c XOR true` | One-bit result |

These are value rewrites. They do not prune a CFG edge or make an effectful
arm-producing definition disappear.

## SSA simplification and propagation — 3

Files: `kernels/local/propagation/<hyphenated-kernel-name>.lua`.

| Kernel | Objective |
| --- | --- |
| `phi_constant` | Replace a phi carrying the same integer literal on every incoming edge with a constant; move it after remaining phis. |
| `copy_propagation` | Replace uses of a copy's ValueId by its source operand, including phi-edge uses. Keep the unused copy definition. |
| `constant_propagation` | Substitute a constant definition's integer literal into operand uses. Keep the unused definition; do not fold arithmetic or prune branches. |

## Common-subexpression elimination — 4

Files: `kernels/local/redundancy/<hyphenated-kernel-name>.lua`.

| Kernel | Eligible expressions |
| --- | --- |
| `cse_integer_arithmetic` | Modular add, sub, mul and neg |
| `cse_integer_bitwise` | AND, OR, XOR and NOT |
| `cse_integer_comparison` | Integer eq, ne, lt, le, gt and ge |
| `cse_select` | Select with identical condition and ordered integer arms |

A matching candidate must dominate the duplicate, and must precede it when
both are in the same block. The duplicate becomes a copy of the earlier result.
Canonicalize commutative operands only for add, mul, AND, OR, XOR, eq and ne;
ordered relations and select arms retain their order. No load, store, call,
division, remainder, shift or floating-point expression is deduplicated.

## Constant reassociation — 5

Files: `kernels/local/reassociation/<opcode>-constants.lua`.

| Kernel | Transformation |
| --- | --- |
| `add_associate_constants` | `(x+a)+b -> x+(a+b)` |
| `mul_associate_constants` | `(x*a)*b -> x*(a*b)` |
| `and_associate_constants` | `(x & a) & b -> x & (a & b)` |
| `or_associate_constants` | `(x \| a) \| b -> x \| (a \| b)` |
| `xor_associate_constants` | `(x ^ a) ^ b -> x ^ (a ^ b)` |

Accept either operand order at both nodes, combine constants modulo the declared
width, and retain the inner definition. These rules do not authorize
floating-point reassociation or removing overflow traps.

## Validation

`tests/kernels.cpp` has a positive and nonmatching case for every identifier.
For each positive pattern it checks the reported change, output SSA and analysis
contract, and compares observable returns at 8-, 32- and 64-bit widths under
both signed and unsigned interpretations. Boundary bit patterns and deterministic
generated inputs exercise wraparound and minimum values. Extra cases check
zero divisors, invalid shifts, commuted operands, copy/constant chains, phi
ordering, CSE across dominating and disjoint branches, floating-point exclusions,
custom-named types and retained load evaluation. Every entry also has an
unknown-semantics negative case. The existing strength-reduction suite remains
separate, and the installation suite loads the complete 71-kernel catalog.
