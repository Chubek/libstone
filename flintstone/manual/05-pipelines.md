# 5. Verification, conversion and passes

## Verify at boundaries

```cpp
auto module = flintstone::parse(source, "input.flint");
auto diagnostics = flintstone::verify(module, registry);
if (!diagnostics.empty()) {
    for (const auto& d : diagnostics)
        std::cerr << flintstone::format(d) << '\n';
}
flintstone::require_valid(module, registry); // Throws Error with diagnostics.
```

Verification checks module/target/type/attribute shape, registry membership,
function symbols and signatures, external declarations, operation levels and
arity, CFG structure, phi predecessor coverage, SSA uniqueness, dominance,
literal ranges and operation-specific contracts. Firestone's CFG dominators
are reused for dominance legality. Scalar export additionally passes
Firestone's SSA verifier.

Locations carry file, line and column. Diagnostics can also carry function,
block ID and operation index. Programmatically constructed objects may have no
source location. Parsing preserves input locations; conversion rules should
copy the original operation/result locations to their replacements.

## Explicit conversion

`instantiate(module, registry)` returns a verified, concrete copy with type and
index substitution applied. `lower(module, registry, destination, budget)`
specializes, repeatedly applies registered rules until every operation is legal
at the destination, verifies the result and then commits it.

```cpp
flintstone::lower(module, registry, flintstone::Level::Core);
```

`High -> Core`, `Core -> Machine`, and `High -> Machine` are supported directions.
Raising a module to a higher abstraction is not implicit. A missing conversion,
invalid output or exhausted rewrite budget leaves the original module intact.
The default budget is 100000 rule applications. A rule that keeps reproducing
an illegal operation eventually fails; there is no unbounded fixed-point loop.

Rules are selected by exact source operation and destination stage. Registration
order resolves competing rules. Returning an empty vector erases an operation;
that is legal only when it has no original results. Every original result ID
and type must be defined exactly once in the replacement. The final verifier
checks any additional results, references and control flow.

### Extend the offset dialect

After registering the `toy` dialect from the previous chapter:

```cpp
registry.add_lowering({
    "toy-offset-to-add", "toy.offset", flintstone::Level::Core,
    [](const flintstone::Operation& source, const flintstone::Function&,
       const flintstone::Module& module, const flintstone::Registry& registry) {
        auto replacement = source;
        replacement.name = "arith.add";
        auto parameters = registry.parameters(module, "toy");
        replacement.operands.push_back(flintstone::Operand::constant(
            std::get<std::int64_t>(parameters.at("amount")),
            source.results.at(0).type));
        return std::vector<flintstone::Operation>{replacement};
    }
});
```

For an expansion needing temporaries, `Function::fresh_value()` returns a free
ID; subsequent temporary IDs in the same replacement must also be distinct.
The callback receives a const function/module. It returns replacements rather
than mutating the driver's block vector. For CFG restructuring, use a verified
module pass instead of invalidating conversion iterators.

The builtin `math.madd` rule expands into a multiply defining a temporary and
an add defining the original result ID. `math.square` duplicates its input;
`math.scale` introduces the resolved dialect factor.

## Transactional pass pipelines

```cpp
flintstone::PassManager manager;
manager.add(flintstone::blackstone_pass(
    "/path/to/blackstone/kernels", {"add_zero", "mul_one"}));
manager.add(flintstone::dead_code_elimination());
auto reports = manager.run(module, registry);
```

Lower high-level operations before invoking Blackstone. Each report contains
the pass name, actual change flag and preservation declaration. The entire
manager run is transactional: if the third pass fails, the first two passes'
changes are also discarded. Input and each intermediate result are verified.
Pass failures keep the original structured diagnostics with a `pass.<name>`
prefix, so an `ssa.*` or `operation.*` cause is not hidden behind a generic
pass error. An integer module revision advances for committed changes and
specialization; it is not serialized as program semantics.

A pass callback returns whether it changed the IL. The driver compares canonical
program text and rejects inconsistent change claims. Locations/revisions are
not part of that semantic-text comparison. Passes should not use false returns
to hide semantic changes.

`dead_code_elimination` repeatedly removes unused result-producing pure
operations. It retains reads, writes, calls, terminators and potentially
trapping division/remainder/shifts even when results are unused. It does not
perform whole-program reachability, memory alias optimization or dead-cycle
collection. Purity is supplied by the operation's registered definition.

`vector_scalarize_pass` unrolls vector packs to scalar `arith` operations and
`tuple_sroa_pass` replaces tuple aggregates with their scalar fields (both
handle loop-carried values with per-lane/field phis). Either fails with
`vector.escape`/`tuple.escape` when a value would cross the function boundary.
Run them before `lower()` (or use `--scalarize`); afterwards the function is
ordinary scalar code that converts, optimizes and executes normally.

## Analyses and preservation

The default analysis names and `std::any` payloads are:

| Name | Payload |
| --- | --- |
| `def_use` | `flintstone::DefUse`, keyed by `(function name, value ID)`. |
| `cfg` | `std::map<std::string, firestone::ir::ControlFlowGraph>`. |
| `dominators` | `std::map<std::string, firestone::ir::DominatorSets>`. |
| `loops` | `std::map<std::string, std::vector<firestone::ir::NaturalLoop>>`. |

The structural analysis graph uses stable dense block IDs in declaration order;
it is not a copy of scalar operations. It is sufficient for CFG, dominance and
natural-loop analysis. Use the module's block declaration order to map these
dense IDs back to external block IDs.

A pass declares required analysis names and preserved names. Requirements are
materialized before it runs; unknown names fail explicitly. A changed pass
invalidates cached analyses not preserved. A preservation claim is a host
contract, not an automatic proof that arbitrary custom analysis data remains
correct.

Add custom factories with:

```cpp
manager.add_analysis("function_count", [](const auto& module, const auto&) {
    return std::any(module.functions.size());
});
```

The manager creates a fresh cache for each run. Do not retain references to
cached payloads across invalidating passes. Synchronize access to a shared
mutable manager/module.

## Blackstone boundary

Flintstone loads the requested kernel directory through `blackstone::Runtime`.
Names must exist in the SSA catalog; unknown names fail even for an empty
module. Each function is exported to verified Firestone SSA. Blackstone runs
with explicitly modular integer semantics, and its own transactional adapter
checks transformed SSA before import. Flintstone then verifies the imported
function/module before the outer pass transaction commits.

The import retains original value/block identities and source locations where
available; newly allocated recurrence values receive fresh IDs. Blackstone's
current adapter preserves the CFG. Unsupported scalarization, custom types,
multi-result operations or missing semantic bridges are errors, not reasons to
skip a function silently. Kernel directories contain executable Lua application
code; do not load untrusted catalogs.

Next: [Execution](06-execution.md).
