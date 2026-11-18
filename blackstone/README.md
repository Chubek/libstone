# Blackstone 0.2

Blackstone is a Lua optimization-kernel catalog with an `lrewrite` execution
interface. Kernels declare their identity, IR, phase, analysis requirements,
preservation contract and transformation. C/C++ hosts can discover kernels and
provide execution contexts. The supplied Firestone adapter implements the
object protocol used by `kernels/local/strength-redudction.lua`.

The catalog contains **191 executable kernels**: the 71 original kernels and
120 extended integer SSA rewrites. See [the kernel inventory](docs/kernels.md)
and [the extended rule manifest](docs/extended-kernels.json).

## Build

```sh
cmake -S blackstone -B build-blackstone
cmake --build build-blackstone --parallel
ctest --test-dir build-blackstone --output-on-failure
build-blackstone/blackstone-kernels blackstone/kernels --ir ssa
```

From the repository root, the aggregate build selects Blackstone with
`LIBSTONE_BUILD_BLACKSTONE`. Standalone options are `BLACKSTONE_BUILD_TESTS`
and `BLACKSTONE_BUILD_TOOLS`. Dependencies are the existing Firestone headers
and vendored Lua sources (`LIBSTONE_LUA_SOURCE_DIR`). The Lua interpreter is
built locally; configuring never downloads dependencies. Blackstone requires
C++17 and Lua 5.3 or newer. The installed CMake package exports
`Blackstone::blackstone`, depends on `Firestone`, and supplies
`Blackstone_KERNEL_DIR`. Both packages must be discoverable by the consumer's
CMake configuration; they can use separate install prefixes.

## Kernel format

Each `.lua` file declares exactly one kernel and supplies a run function:

```lua
lrewrite.kernel.define {
    name = "my_optimization",
    ir = "ssa",
    phase = "local_optimization",
    requires = { "def_use" },
    preserves = { "cfg" },
    documentation = "Describe the transformation, assumptions and limitations."
}

lrewrite.kernel.run = function(ctx)
    -- Inspect and transform the host's context.
    return false -- strictly boolean: whether the representation changed
end
```

`name`, `ir` and `phase` must be nonempty strings. `requires` and `preserves`
must be dense lists of unique nonempty strings, including when empty.
`documentation` is optional; if present it is a nonempty string. Unknown fields,
duplicate names, repeated declarations, missing run functions and malformed
metadata are errors. Names are stable catalog identifiers, independent of paths.
IR, phase and analysis identifiers are extensible strings; the host must supply
every required analysis and match the declared IR exactly.

Each kernel has an independent global environment and kernel slot. Lua standard
libraries are shared and available. Load kernel files as application code.
Recursive directory discovery selects `.lua` files in sorted path order. Loading
a directory is atomic for the catalog: if any file fails, all entries added by
that operation are removed. File paths and Lua line diagnostics appear in errors.

`blackstone-kernels DIRECTORY [--ir IR | --phase PHASE]` validates and lists a
catalog with its requirements, preservation contracts and documentation.

## C++ / Firestone

```cpp
#include <blackstone.hpp>

blackstone::Runtime runtime;
runtime.load_directory("blackstone/kernels");
for (const auto& kernel : runtime.kernels()) {
    // kernel.name, .ir, .phase, .requirements, .preserves, .documentation
}

// graph is a firestone::ir::SimpleSSAGraph with valid CFG/SSA.
auto result = runtime.run("strength_reduction", graph,
                         blackstone::IntegerSemantics::Modular);
// result.changed; result.invalidated lists supplied analyses not preserved.
```

The adapter verifies input SSA, computes natural loops, definitions and
dominators, and executes the Lua kernel on an independent snapshot. It commits
only after the kernel reports a change, the snapshot has a valid structure, and
the transformed graph passes Firestone's verifier. A false result or an
exception leaves the original graph untouched. The adapter supports
instruction/operand rewrites and integer value allocation while retaining CFG
blocks, edges and terminator kinds. Graph-level transformations need a
different host context.

### Bounded pipelines

```cpp
auto report = runtime.run_pipeline({"mul_zero", "fold_add", "ext_zero_minus_x"},
                                   graph, blackstone::IntegerSemantics::Modular,
                                   /*max_rounds=*/4);
```

`run_pipeline` validates the requested kernels before starting and applies them
in the supplied order, refreshing the SSA snapshot for each invocation.
`report.invalidated` is the union from kernels that changed the graph.
`report.rounds` counts completed sweeps and `report.converged` means a full sweep
reported no changes. The round limit defaults to one and must be positive; if the
last permitted sweep reports a change, the pipeline commits the verified result
with `converged == false`. An empty pipeline converges in zero rounds.

An exception rolls back the **entire graph**, including changes from earlier
kernels in the pipeline. It does not roll back Lua state or external effects.
As with `run`, convergence relies on kernels correctly reporting whether they
changed the graph.

### SSA context protocol

- `ctx.loops:all()` iterates natural loops.
- `loop:induction_variable()` recognizes an integer header phi with literal
  zero outside inputs and unit increments on all backedges.
- `loop:instructions()` iterates a stable instruction snapshot, permitting edits
  during traversal. Instructions expose `op` and `result` (`name`, `type`, `id`).
- `inst:match_induction_times_constant(iv)` matches either multiplication
  operand order, with a same-width integer literal constant.
- `ctx:can_strength_reduce(loop, inst, iv, scale)` checks modular semantics,
  loop membership, header dominance and the supported induction pattern.
- `loop:create_recurrence{ name=..., type=..., initial=0, step=... }` creates an
  integer phi. `recurrence:advance_on_backedges()` inserts the corresponding
  additions before each backedge terminator.
- `value:replace_uses_with(other)` rewrites all operand uses with type checking.
- `ctx:erase(inst)` retires a replaced multiplication. Firestone's stable IDs
  require each allocated value to have a definition; the adapter retains a dead
  copy of the recurrence value in that slot. Live uses refer to the recurrence.

The underlying snapshot contains `blocks`, `loop_list`, `dominators`,
`value_types`, `next_value`, `new_types` and `integer_semantics`. Kernels using the
object protocol need not manipulate those tables directly. Block IDs are map
keys; instruction, operand, target, incoming-block and new-type lists must be
dense one-based arrays.

### Strength-reduction contract

The provided kernel supports 2–64-bit modular integer arithmetic, zero initial
induction values, step one and literal scales greater than one. It handles
multiple latches, leaves CFG structure intact, and reports `def_use`,
`dominators` and `loop_info` as invalidated unless declared preserved.
Nonzero initial values, nonunit steps, floats and unknown/trapping overflow
semantics do not authorize a rewrite. Arithmetic semantics default to `Unknown`;
the host explicitly opts into modular arithmetic. Profitability is target-dependent.

## C embedding

`blackstone.h` exposes an opaque runtime, catalog metadata accessors, loading,
execution and `blackstone_last_error`. Operations return zero on success and
`-1` on error. `blackstone_create` returns null if initialization fails.

`blackstone_run` takes the kernel name, host IR, available analysis names and
a callback that pushes exactly one Lua context table. The callback executes
inside a protected Lua call and must not throw a C++ exception. The host owns
the context's lifetime and rollback behavior. Use the C++ Firestone entry point
for transactional SSA rewrites. A runtime is single-threaded and non-reentrant;
use separate runtimes for concurrent hosts.

Installed Lua headers and the interpreter target are available for embedding:
link `Blackstone::lua` when a host directly uses the Lua C API. Catalog strings
are borrowed and remain valid until the next successful load or destruction.

## Pattern interoperability

`blackstone::parse_pattern` and `blackstone::match` reuse Firestone's term
parser and matcher. Variables are named leaves such as `?x`; repeated variables
must match equal subterms. Lua kernels can call:

```lua
local bindings = lrewrite.match("(add ?x ?x)", "(add a a)")
assert(bindings["?x"] == "a")
```

A mismatch returns `nil`, successful bindings are serialized Firestone terms,
and malformed patterns produce an error.

## Integer local kernels

The 70 original local optimizations are grouped under `kernels/local/`:

| Family | Kernels | Phase |
| --- | ---: | --- |
| Constant folding | 18 | `constant_evaluation` |
| Algebraic identities and cancellation | 29 | `algebraic_simplification` |
| Reflexive comparisons | 6 | `comparison_simplification` |
| Select simplification | 5 | `select_simplification` |
| Phi simplification and value propagation | 3 | `ssa_simplification`, `value_propagation` |
| Dominance-aware common-subexpression elimination | 4 | `redundancy_elimination` |
| Constant reassociation | 5 | `integer_reassociation` |

Each file declares one independently discoverable kernel and binds its run
function with `lrewrite.local_rewrite("rule_name")`. The shared factory in
`lrewrite/local.lua` implements width normalization, definition lookup,
applicability checks and the named rule. An unknown rule name is a load error.
This helper is embedded alongside the main Lua library and installed with it.

These kernels require explicit `IntegerSemantics::Modular`, valid typed SSA,
and built-in integer types of width 1–64 with no custom type name. The snapshot's
`value_types` table supplies declared types for both parameters and definitions,
so a Boolean comparison result does not make its floating-point operands eligible.
Constants may be recovered through constant/copy definitions; missing facts
remain unknown. Narrow constants are normalized to their width, then emitted
as sign-extended bit patterns, except Boolean constants, which remain zero or one.

Division/remainder folding requires nonnegative, signed-representable operands
and a positive divisor. Ordered-comparison folding uses the same common
signed/unsigned domain. Right-shift folding requires a clear sign bit; all folded
shift counts must be within the type width. These restrictions avoid inventing
signedness or invalid-operation semantics absent from Firestone's types.

Each of these kernels preserves CFG and dominators while conservatively invalidating
def-use and induction facts. Operand-producing instructions are retained, even
when their result becomes unused: simplifying multiplication by zero must not
delete a load, call or potentially trapping input computation. Common-subexpression
elimination only reuses pure operations with a dominating, earlier definition;
it excludes memory, calls, division and shifts. These local kernels add no CFG
rewrites or new values, and error handling uses the existing transactional adapter.

## Extended integer kernels

`kernels/extra/` adds 120 independently loadable rules across arithmetic,
bitwise simplification, equality comparisons, selects, factoring, constant
reassociation, and shifts. For example, `ext_factor_bitwise_or_of_ands` rewrites
`(x & y) | (x & z)` to `x & (y | z)` even if the operands of the nested ANDs
are commuted. Rules match same-width unnamed integers with explicit modular
semantics. They retain the original result IDs and operand producers, preserve
CFG and dominators, and can insert integer intermediates of width 2–64.

The rule generator and differential fixtures can be regenerated from the
repository root; Python is not required for building or using Blackstone:

```sh
python3 -B blackstone/tools/extended/generate.py
python3 -B blackstone/tools/extended/make_tests.py
```

## Validation

The runtime tests exercise C embedding, catalog validation, deterministic and
atomic directory loading, independent globals, pattern matching, missing
analyses, IR mismatches and rollback on Lua errors and invalid output SSA.
Differential tests compare original and reduced return values over 8-, 32- and
64-bit arithmetic, zero-trip loops, modular overflow and multiple backedges.
Negative cases include unsupported initial values, strides, scales and arithmetic
semantics. These checks are executable evidence for the supported contract.

`blackstone.kernels` checks all 70 original local optimizations with positive and
nonmatching cases, unknown-semantics rejection, valid output SSA and analysis
contracts. Differential evaluation compares original and rewritten return values
at 8-, 32- and 64-bit widths under both signed and unsigned interpretations,
using boundary bit patterns and deterministic generated inputs. Additional cases
cover wraparound, minimum values, zero divisors, invalid shifts, commuted operands,
cross-block CSE dominance, phi ordering, floating-point/NaN-sensitive exclusions,
custom types and retained effectful inputs. `blackstone.extensions.lua` runs
76,800 signed/unsigned differential checks over all 120 extended rules at 8,
16, 32, and 64 bits. `blackstone.extensions.ssa` checks representative cases
through the native adapter, including nested commutation and one-bit rejection
for a rewrite requiring a temporary. The installation check loads all 191
kernels and runs a pipeline through an installed C++20 consumer.
