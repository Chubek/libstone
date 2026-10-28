# 11. Firestone code-generation plans

Include `<targets/Firestone.hpp>` and link `Flintstone::codegen` to use
Firestone's instruction selection, scheduling and register allocation directly:

```cmake
find_package(Flintstone CONFIG REQUIRED COMPONENTS codegen)
target_link_libraries(my_compiler PRIVATE Flintstone::codegen)
```

This C++ interface produces an inspectable, verified **plan**, not executable
machine code. It complements the [Limestone/Sandstone native adapter](07-native-targets.md);
it does not change existing VM, native, CLI or C ABI behavior.

## Minimal example

```cpp
#include <targets/Firestone.hpp>

flintstone::Registry registry;
auto module = flintstone::parse(R"(module demo level core {
  func @increment(%x:i32)->i32 {
    ^entry: %r:i32=arith.add(%x,1:i32); func.return(%r);
  }
})");
flintstone::FirestoneTarget target;
auto rule = [&](const char* pattern, const char* opcode) {
    firestone::isel::Rule r;
    r.name = opcode;
    r.instruction = opcode;
    r.pattern = firestone::rewrite::parse(pattern);
    target.rules.add(r);
};
rule("parameter", "ARG");
rule("constant", "IMM");
rule("(add ?lhs ?rhs)", "ADD");
target.scheduling.issue_width = 2;
target.scheduling.resources = {{"alu", 2}};
target.registers = firestone::regalloc::Machine::uniform(4);
auto plan = flintstone::plan_firestone(module, module.function("increment"), registry, target);
```

The example's uniform registers and unit-latency ALU rules are illustrative
target facts, not a description inferred from the host CPU. Real targets supply
rules with type/immediate guards, costs, latency, resources, occupancy, unit
counts and explicit operand order. Rules must implement Flintstone's modular
integer semantics; the planner does not authorize trapping-overflow instructions
or fast-math reassociation. Argument/return conventions remain target-owned.

## Region export and provenance

`firestone_region(module, function, registry)` specializes and lowers high-level
operations to core in a private copy, then uses the existing verified SSA bridge.
The function reference must belong to `module`. Neither export nor planning
changes the source module or its revision.

The supported region is one scalar block ending in a return. Every non-return
operation must be registered as pure, non-phi and non-terminating. Calls, loads,
stores, division, remainder, shifts, global-symbol reads and CFG/phi functions
fail explicitly, even when their results are unused. Aggregate types need the
existing explicit scalarization passes first. General loops/effects continue
to use the SSA/VM or native-adapter paths; no side effect is dropped to fit a DAG.

`FirestoneRegion` exposes:

- `graph`: a Firestone selection DAG, preserving shared values;
- `roots`: the returned value, or no roots for a void return;
- `values`: original Flintstone value IDs (plus any lowering temporaries) mapped
  to selection node IDs. Never assume those ID spaces coincide.

Parameters are typed `parameter` leaves. Numeric literals are `constant` leaves
whose `Node::source` contains their original typed operand. Other nodes use
Firestone opcode names and retain the original SSA instruction in `source`;
all operands, including inline literals, become explicit DAG inputs. A target
guard can inspect this metadata rather than guess an immediate from a name.
Unused pure nodes may be omitted by selection. All graph nodes are still checked
for unsupported effects/symbols before selection.

## Algorithms and verification

The defaults call Firestone's `BURS`, `ListSched` and `GreedyRegalloc`.
`FirestoneTarget::select`, `schedule` and `allocate` callbacks let a host select
other Firestone algorithms without a second solver implementation. For example:

```cpp
#include <libisel/GraphCovering.hpp>
#include <libregalloc/PBQPRegalloc.hpp>
target.select = [](const auto& graph, const auto& rules, const auto& roots) {
    return firestone::isel::GraphCovering{}.select(graph, rules, roots, 10000);
};
target.allocate = [](const auto& problem, const auto& machine) {
    return firestone::regalloc::PBQPRegalloc{}.allocate(problem, machine, 10000);
};
```

Each callback result is independently verified against its input. Infeasible,
incomplete, malformed or falsely optimal results fail with `firestone.selection`,
`firestone.schedule` or `firestone.allocation`. Feasible `SearchLimit` incumbents
are accepted **without** changing their stop/optimality flags. Exact-search
budgets belong to the chosen callback; heuristic defaults claim no exact search.

The plan owns the selection, scheduling graph and schedule, schedule-derived
allocation problem, register-pressure profile and verified allocation. The
allocation problem's `values` and `sources` maps connect allocation IDs back to
selection IDs, which in turn connect to the exported Flintstone IDs. Live ranges
use Firestone's read-before-write cycle slots and keep return roots live to exit.
`configure_value` may set classes, banks, fixed/allowed registers, spill costs and
layout; the adapter retains the schedule-derived ranges and uses.

Stack slots and rematerialization markers are valid planning outputs, not
inserted spill/reload code. Native encoding, tied/early-clobber constraints,
spill materialization, frame layout and ABI lowering still require a target
adapter. Do not pass the plan off as a complete linked native image.

## Blackstone first

Blackstone remains the optimization catalog. To optimize before planning:

```cpp
flintstone::lower(module, registry, flintstone::Level::Core);
flintstone::PassManager passes;
passes.add(flintstone::blackstone_pass(kernel_directory, {"add_zero", "mul_one"}));
passes.add(flintstone::dead_code_elimination());
passes.run(module, registry);
auto optimized = flintstone::plan_firestone(module, module.function("increment"), registry, target);
```

The existing Blackstone boundary explicitly requests modular integer semantics
and verifies the rewritten SSA before import. The planner consumes those
committed results, rather than maintaining a competing rewrite catalog or
silently running an unknown set of kernels. Rebuild a plan after any mutation;
plans are snapshots, not automatically invalidated analysis caches.
