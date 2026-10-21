# 6. Targets, transformations, and extension points

## Semantic rules before optimization

A correct transformation needs more than a matching opcode name. Establish:

- operand/result types and widths;
- signedness, overflow, division, shift, and floating-point rules;
- memory effects, trapping behavior, and observable calls;
- dominance, control-flow, and phi-edge requirements;
- which analyses remain valid after the change.

Firestone's generic rewrite/e-graph engines install no implicit algebraic
equalities. Blackstone kernels carry more specific contracts. Flintstone's
built-in integer semantics are modular and width-limited, while plain
Firestone VM scalar arithmetic is checked; bypassing Flintstone's runtime
intrinsics can change observable behavior.

## Add a Blackstone kernel

Each Lua file declares one kernel and a run function:

```lua
lrewrite.kernel.define {
    name = "my_optimization",
    ir = "ssa",
    phase = "local_optimization",
    requires = { "def_use" },
    preserves = { "cfg" },
    documentation = "State exact preconditions and the supported rewrite."
}

lrewrite.kernel.run = function(ctx)
    -- Inspect the host's context; return true only after a real change.
    return false
end
```

Names, IR, and phase must be nonempty; requirement/preservation arrays must
contain unique nonempty strings. The return value is strictly Boolean. The
host must match the IR and supply required analyses. Directory discovery is
sorted and catalog loading is atomic on failure.

The Firestone adapter supports operand/instruction edits and integer value
allocation while retaining CFG structure and terminator kinds. A CFG-restructuring
kernel needs another host context. The strength-reduction kernel additionally
requires modular arithmetic and its documented zero-start, unit-step induction
pattern; it is not a general loop optimizer.

Validate new kernels with positive matches, nonmatches, unsupported semantics,
rollback, verifier failures, and differential execution. See the [Blackstone
guide](../blackstone/README.md) and [kernel contracts](../blackstone/docs/kernels.md).

## Extend Flintstone dialects and passes

Flintstone registers parametric types, operations, dialect configuration, and
targets. Its shipped dialects include arithmetic, control flow, functions,
memory, math, vectors, tuples, pointers, atomics, and VM operations. A dialect's
presence does not imply every operation is executable on every target.

A useful extension has a parser/verifier contract, explicit parameter
specialization, legal source/target levels, and conversion behavior. Preserve
source diagnostics and type identity. Reject unsupported operations rather than
renaming them into superficially similar backend instructions.

Passes should validate input and output and commit only successful conversions.
Aggregate elimination is explicit scalarization. Native lowering must not
silently fall back to VM execution or infer an ABI from a target string.
Use the [dialect/type guide](../flintstone/manual/04-dialects-and-types.md),
[pipeline guide](../flintstone/manual/05-pipelines.md), and
[native adapter guide](../flintstone/manual/07-native-targets.md).

## Native targets

A usable native backend must supply and validate all relevant layers:

| Layer | Required facts |
| --- | --- |
| Metadata | Registers, instruction forms, features, operand constraints, provenance |
| Selection | Semantic patterns and complete coverage of supported operations |
| Scheduling | Semantic dependencies, latency/resource model, legal motion |
| Allocation | Register classes, fixed/tied operands, interference, spill storage |
| ABI | Arguments/results, caller/callee saves, stack alignment, frame layout |
| Encoding | Exact forms, immediate ranges, byte order, fixups/relocations |
| Packaging/runtime | Object format, symbol resolution, executable ownership and host compatibility |

Limestone's Metacode Infobank is authoritative input for its target properties.
Do not invent missing properties or conflate scheduler-only constraints with
semantic dependencies. A spill decision also needs materialized loads/stores
and storage; renumbering virtual values is not a complete spill implementation.

Sandstone supplies separate small `.isa` descriptions and SymAS forms. A bridge
must explicitly map the operation, operands, register names, and relocations it
uses. Matching architecture labels across libraries do not prove matching
semantics or an ABI.

Flintstone ships tiny native targets for x86-64, AArch64, and RISC-V, plus a
generic Sandstone bridge and architecture registry. Tiny-target coverage is
deliberately restricted; unsupported CFGs, types, or ABI behavior must be
diagnosed. Metadata coverage is broader than executable backend coverage.

See [Limestone target development](../limestone/manual/21-targets-backends-and-adapters.md)
and [implementation status](../limestone/IMPLEMENTATION.md) before promising
support for a new target.

## Binding extensions

An XBind spec is executable Lua returning an `xbind.library` manifest. Declare
the actual C headers, exact signatures, target options, and explicit type maps.
`--check-spec` checks shape; compiler-generated signature probes check the ABI
declarations. Neither performs automatic header parsing.

Current Lua lowering handles scalars, borrowed strings, and explicitly mapped
opaque pointers. Unsupported callbacks, records, automatic buffers, and ownership
marshalling must fail rather than emit unsafe placeholders. The other eight
listed language targets are not implemented emitters. Follow the [XBind
manual](../tourchstone/xbind/MANUAL.md) when adding a spec or extending lowering.

Next: [Testing and troubleshooting](07-validation.md).
