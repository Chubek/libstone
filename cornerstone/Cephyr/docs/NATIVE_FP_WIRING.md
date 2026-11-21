# Native C11 scalar floating-point slice

Cephyr lowers the scalar `float`/`double` subset described below to typed
native MachineIR on the reviewed AMD64/SysVAMD64 and little-endian
AArch64/AAPCS64 providers. There is no host C code generation fallback.
The bundled RV64IM LP64 route uses integer MachineIR for its soft-float
subset; it does not emit hardware FP instructions. Bootstrapping and
regression details are included below.

Since the initial slice documented below, both supported providers also fold
`float` and `double` arithmetic in static initializers using target-width
round-to-nearest-even evaluation. Both also support direct scalar variadic
calls with default promotions and scalar variadic definitions. RV64IM
supports storage, fixed-prototype transport, negation, and comparisons for
`float` and `double`, but not general evaluated arithmetic or conversions.
AMD64 supports x87 `long double` objects and constant initialization, not
evaluated runtime scalar operations or value transport. Annex F remains
unsupported.

## Slice 1: capability, source types, and literal spelling

- **`include/cephyr/preprocess.hpp`, `src/main.cpp`:** Add
  `Preprocessed::native_scalar_fp`, default `false`. In the C/`.i` input
  branch beside `prep->plain_char_signed`, set it only for the validated
  built-in AMD64/SysVAMD64 and AArch64/AAPCS64 routes. `provider` has already
  been cleared for noncanonical spec paths; also exclude separately selected SDK
  backends unless they have their own declared FP capability. Carry the
  decision into `--emit-mir` and syntax-only just like `-S`. Do not use host
  architecture, target triple prefix, user macros, or a toolchain fallback.
- **`include/cephyr/lower.hpp`:** Add `CType::Kind::real`, `real()` and
  `arithmetic() == integer() || real()`, and include real in `scalar()`.
  Declare `real_type(unsigned bits)` and `common_arithmetic(...)`. Keep
  `Expr::constant` for integer constant expressions/null recognition, not raw
  FP bits. Declare a shared `real_constant(Node *, const TypeContext &)`
  returning `Constant{real_type(...), IEEE_bits}` so expression and static
  initialization use exactly the same diagnostics and representation.
- **`src/semantics.cpp`:** In `read_type`, separate `NK_REAL_TYPE` from
  `NK_COMPLEX_TYPE`. Read the parser's `RealTypeASTNode`: `(4, false)` means
  float, `(8, true)` double, and `(16, true)` long double (diagnose rather than
  approximating). Use `qualify`. Reject real declarations with
  `!source.native_scalar_fp` and report a hardware FP ABI requirement.
  `real_type` constructs only 32/64; `compatible` compares width (and already
  checks qualifiers); `size_of`, `align_of`, and `mir_type` report 4/8, 4/8,
  and `f32`/`f64`. `common_arithmetic` picks the widest real operand if both
  operands are arithmetic and either is real; otherwise delegate to
  `common_integer`. **Leave `promote` integer-only** to avoid silently
  enabling bitwise operations, shift counts, array indices, or integer
  constant expression paths. In `compatible`'s prototype/no-prototype
  branch, a `float` parameter is not compatible after default promotions
  (it becomes `double`). Calls without a prototype still remain unsupported.
- **`src/floating_literal.hpp` (implemented in isolation):** Parse the
  `RealConstASTNode::getSpelling()` token, never `getValue()`. Pick nominal
  format from the suffix, decode exactly one token, and obtain bits directly
  in that format without going through `long double`. The decoder uses
  locale-independent `from_chars` on guarded IEEE hosts. It returns separate
  malformed, unsupported-format, size-limit, and overflow statuses; accepted
  underflow-to-zero gives `+0` until unary minus is applied. Static assignment
  conversion helpers operate on bits, including integer conversion, to avoid
  a host rounding-mode dependency. The subset uses no excess precision.
  Reuse this parsing plus `floating_literal_to_integer` for
  `cast_real_literal` instead of host `long double` arithmetic. Preserve the
  existing integer-only fold of a direct `(int)3.5` on RV64 (C11 6.6p6); it
  requires no floating value or ABI. A standalone real expression or real
  type on that target still diagnoses.
- **`third_party/cparser-iso11`:** `CParser::UnaryExpression` currently
  rewrites unary `-` as binary `0 - operand`. The least-invasive patch is
  an `isUnary()` flag on `MinusExprASTNode`, defaulting to false, set only
  when `UnaryExpression` constructs that synthetic subtraction. Keep the
  existing integer ICE and runtime paths; inspect the flag for FP lowering
  and static initializers. Never infer it from a literal zero left
  operand. `-0.0` and `0.0 - 0.0` differ. `CLexer::readNumber` calls host
  `strtold`; do not treat host `ERANGE` as a target range error. Ultimately
  validate only token syntax in the lexer and let Cephyr translate the
  spelling; a temporary `strtold` value for legacy AST consumers does not
  control the target result.

## Slice 2: typed expressions and fixed-signature calls

All changes here fit in **`src/lower_expr.cpp`** after the type changes.

- `expression_type`: type `NK_REAL_CONST` through the shared literal
  function. Accept arithmetic-to-arithmetic assignment; `_Bool` may still
  take pointers. Reject explicit real ↔ pointer casts when probing types,
  as well as in `convert`. `arithmetic_type` and `conditional_type` use
  `common_arithmetic` for the appropriate operations. Keep `%`, bitwise
  operators, `~`, and shifts integer-only. `+`/`-` unary preserve nominal FP
  width; permit real operands for increment/decrement if exposing them.
  Real comparisons type to `int`, and existing scalar logical rules apply.
- `lower_expr`: emit `fconst` with the decoded bit pattern, and `fneg` for
  unary minus (which is not subtraction). Unary plus returns the FP
  operand unchanged. Keep the existing direct real-literal-to-integer
  ICE case. `/` is the parser's `NK_TRUNC_DIV_EXPR` for both integers and
  real; select from operand types, not that AST name.
- `load`/`store`: add `fload DST PTR 0` / `fstore PTR SRC 0` branches beside
  the pointer cases, after existing volatile/const checks. Generic lvalue,
  local storage, typed record members, and pointer arithmetic already use
  `size_of`/`align_of`; no alternate object model is needed.
- `convert`: handle `_Bool` first, using `truth`. For real → real emit
  `fcast` only when widths differ; reuse the ID otherwise. For integer →
  real canonicalize the **source** to i64 by declared sign/width then use
  `sitofp`/`uitofp`. For real → non-Boolean integer use `fptosi` or
  `fptoui` based on the **destination** signedness and canonicalize the
  integer result to the destination width. C11 defines truncation for
  representable truncated values, not wraparound for out-of-range inputs.
  No FP bitcasts to/from i64 or pointers.
- `truth`: evaluate `!feq(value, fconst(+0, same type))`. Both zeros are
  false; NaNs are true. `arithmetic_binary`: use `fadd/fsub/fmul/fdiv`
  at the common width. `comparison_operands` converts both arithmetic
  operands; `comparison` uses `feq`/`flt`/`fle` for real, swaps the
  operands for `>`/`>=`, and only inverts `feq` for `!=`. Inverting
  `flt` for `<=`/`>=` incorrectly accepts unordered comparisons.
- `direct_call` and `indirect_call` already convert to declared parameter
  types, and `abi_value`/`canonicalize` leave non-integers unchanged.
  Use the existing functions, signature aliases, result scalar check, and
  `mir_type` serialization. For this fixed-prototype slice, reject
  any attempted variadic call with a real fixed parameter, result, or
  trailing argument; leave existing integer/pointer variadic behavior
  unchanged. Do not add a default float promotion to every fixed call.
  `lower_stmt.cpp` should need no edit for ordinary scalar parameters,
  local initialization, and returns: these already use `scalar()`,
  `store`/`convert`, and `abi_value`. `lower.cpp::write_instruction`
  recognizes FP immediates and arities when remapping SSA IDs after pruning.

## Slice 3: static objects, diagnostics, and execution

- **`src/lower_globals.cpp`:** Dispatch real types in `initialize_scalar`
  and include real in `zero_representable`. Implement `initialize_real`
  beside `initialize_integer`: unwrap braces with `scalar_initializer`,
  accept a real literal or unary signed real literal and integer constant
  expression, convert via `convert_floating_literal` or
  `integer_to_floating_literal`, and write exactly `size_of(type)` bytes with
  `write_integer`. Negate by flipping the source sign bit before conversion;
  preserve negative zero. A float object initialized by an unsuffixed
  literal must round to double first, not parse directly as float. Diagnose
  unsupported static FP constant expressions explicitly (e.g. `1.0 + 2.0`
  in this initial slice) and reject runtime expressions like function calls.
  Reuse existing byte images, static-local path, aggregate scalar dispatch,
  alignment, and ELF emission. Do not hand an arbitrary C initializer to GAS.
- **Tests:** `tests/floating_literal.cpp` is standalone C++20 and registered
  in CMake. `tests/test_cephyr_floating.py` plus
  `tests/fixtures/floating/` checks typed MIR, compiles objects using Cephyr, links an
  independent C ABI caller, and executes on native AMD64 and AArch64
  under QEMU. It distinguishes unary minus from subtract, covers mixed
  conversions, comparisons/truth including NaN, global rounding,
  fixed-signature external/indirect calls, and atomic RV64 rejection for
  both MIR and assembly. The CLI regression's atomic-output check uses
  unsupported `long double`. The bundled `difftime` declaration is gated
  by `__CEPHYR_HAS_SCALAR_FP`; the bundled runtime smoke test calls it when
  that gate is enabled.

Not in the initial slice: long double, complex/imaginary arithmetic, general
static FP constant expression evaluation, FP variadics, a source fenv mode,
Annex F guarantees, or a RISC-V FP/soft-float implementation.
