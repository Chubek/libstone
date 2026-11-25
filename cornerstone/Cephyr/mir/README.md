# Cephyr MIR catalogs

`*.mirpass` and `*.mirlower` are versioned JSON build inputs. Cephyr does not
read, parse, or execute them at runtime. To generate the self-contained C++17
catalog header:

```sh
python3 cornerstone/Cephyr/mir/compile_catalog.py \
  --catalog cornerstone/Cephyr/mir --output build/mir_catalog.hpp
```

## Optimization passes

A pass has `version: 1`, an `id` matching its filename stem, a `summary`,
`min_opt` (1–3), and nonempty `rules`. A rule has `opcode`, `match`, and
`replace`. Opcodes are the wire spellings in [Archon's MachineIR v1
contract](../../Archon/MachineIR.md); for example:

```json
{"opcode": "sub", "match": "RhsZero", "replace": "Lhs"}
```

All rules are for verified `(i64, i64) -> i64` nodes. `LhsZero`, `RhsZero`,
`LhsOne`, `RhsOne`, `LhsAllOnes`, and `RhsAllOnes` test literal 64-bit
patterns; `SameOperands` requires identical SSA definitions.
`BothConstants` requires both literal operands. `Lhs` and `Rhs` reuse that
operand; `Zero`, `One`, `AllOnes` and `Fold` produce literal words.
The generator rejects duplicate opcode/match pairs and all equations outside
its reviewed allowlist.

`cephyr::mir::catalog::rewrite_binary(opcode, all_i64, same_operands,
lhs_const, rhs_const, optimization_level)` returns an optional replacement
(`ReplacementKind::Lhs`, `Rhs`, or `Constant` with `bits`). The caller may
supply a constant only if its definition dominates the use. A single
forward walk with block-local facts is sufficient; absent facts remain
unknown. Apply the result in place as `copy` or `const`, keeping the
destination, all operand producers, CFG and effect order. Do not use this
catalog as permission to remove unused calls, loads, or checked operations.

Catalog level 0 (`--no-mir-opt`, the Cephyr default) uses no rules.
Level 1 describes local identities. Level 2 (`--mir-opt`) also evaluates
two-literal arithmetic, bitwise operations, shifts,
comparisons and unchecked division/remainder. The latter follow Archon's
64-bit wrap, masked shift, and total division semantics, not host or C
source semantics. Floating-point, pointer, checked, and other unsupported
opcodes do not match. A Cephyr configuration can select specific pass IDs
with `[mir]` or `[profiles.NAME.mir]` and `passes = ["add-zero", "fold-bitwise"]`.
Explicit `--mir-opt` and `--no-mir-opt` still choose the full level-2 bundle
or no optimization respectively. The constexpr tables are indexed by
opcode; no pass requires its own graph walk.

| Family | Pass IDs |
| --- | --- |
| Arithmetic identities | `add-zero`, `sub-zero`, `sub-self`, `mul-one`, `mul-zero`, `div-s-one`, `div-u-one`, `rem-s-one`, `rem-u-one` |
| Bitwise identities | `and-all-ones`, `and-self`, `and-zero`, `or-all-ones`, `or-self`, `or-zero`, `xor-self`, `xor-zero` |
| Shift identities | `ashr-zero-count`, `lshr-zero-count`, `shl-zero-count` |
| Comparison identities | `eq-self`, `slt-self`, `ult-self` |
| Literal folding | `fold-bitwise`, `fold-integer-comparisons`, `fold-shifts`, `fold-unchecked-division`, `fold-wrapping-arithmetic` |

## Lowering stages

Every stage is required even at `-O0`. A stage has `version: 1`, `id`,
`kind`, `input`, `output`, and `summary`. These names describe the
accumulated lowering state; they are not separately serialized formats.

| `StageKind` | Responsibility |
| --- | --- |
| `VerifyGraph` | Check the graph, references, supported op layouts and CFG edge shapes. |
| `ProjectTypes` | Map Cephyr types to supported wire carriers without changing conversion semantics. |
| `LinearizeBlocks` | Keep reachable blocks in entry-first source order, preserving each block's instruction order. |
| `AssignWireIds` | Assign dense block and SSA IDs, with function parameters first. |
| `EmitArchonMir` | Emit Archon MachineIR v1 records, preserving simultaneous CFG edge assignments. |

`run_lowering_stages(callback)` invokes the generated stage table in
order and stops on `false`. The graph integration binds every stage to
its implementation and must not publish output if the complete wire
module fails Archon verification.

Run the catalog-local test suite with `python3 cornerstone/Cephyr/mir/test_catalog.py`
(`CXX` selects the C++17 compiler; the default is `c++`).
