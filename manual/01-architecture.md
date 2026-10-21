# 1. Architecture and component guide

## What Libstone provides

Libstone is an aggregate source tree, not one compiler executable or one uniform
C API. Its components expose different representations and can be built
independently. Choose the narrowest component that satisfies your application.

| Component | Responsibility | Main entry points |
| --- | --- | --- |
| **Firestone** | Header-only C++17 IRs, graph analyses, rewriting, equality saturation, CPS, types, runtime foundations, and bytecode VMs | `Firestone.hpp`, `Firestone::firestone`, component targets such as `Firestone::ir` |
| **Blackstone** | Lua optimization-kernel catalog and host execution protocol, with a transactional Firestone SSA adapter | `blackstone.hpp`, `blackstone.h`, `blackstone-kernels` |
| **Flintstone** | C++20 multi-level, parametric-dialect IL; textual frontend, verified conversion, execution, native adapters, C ABI | `Flintstone.hpp`, `Flintstone.h`, `flintstone` tool |
| **Limestone** | Late-stage selection, scheduling, allocation, optimization, binary translation, metatracing, MachineIR exchange, and runtime generation | `limestone/limestone.hpp`, `limestone/limestone.h`, `limestone-cli` |
| **Sandstone** | SymAS databanks, instruction encoding/querying, spec compilation, assembly, and SandSLAB machine-near code generation | `sandstone.hpp`, `sandstone.h`, `sandstone-as`, `sandstone-spec-compiler` |
| **Tourchstone** | XBind specs and Lua generation; optional qBFD-backed dynamic loading and native invocation | `xbind.h`, `xbind`, optional `tdynload` and `dinvoke` |

Firestone and Blackstone require C++17. Flintstone, Limestone, Sandstone, and
XBind's C++ interpreter/generator require C++20. A full build therefore needs
a C++20 compiler as well as a C compiler.

## How the pieces connect

The following shows services and handoffs, not an automatic pipeline available
for every source language and architecture:

```text
Application frontend / textual Flintstone IL
                |
                v
    Flintstone specialization and verified lowering
                |
                +---- Firestone SSA, analysis, bytecode compiler and VMs
                |
                +---- Blackstone optimization kernels
                |
                +---- explicit native target adapter
                              |
                 Limestone scheduling / allocation
                              |
                 Sandstone instruction encoding

Limestone also has its own selection/orchestration pipeline,
TraceML frontend, Bin2Bin tools, Exolayer FFI and VMWeave generator.
Tourchstone XBind describes selected public interfaces for bindings.
```

Firestone supplies reusable representations and mechanisms. Its term-rewriting
and equality-saturation engines do not infer the semantics of arbitrary operator
names. Blackstone adds named transformations with requirements and preservation
contracts. Flintstone supplies an IL whose dialects and targets make those
semantics explicit.

Limestone's fuller backend route is:

```text
Metacode target contracts
          |
program graph -> Tunah -> Unisel / Limeburg -> Schedrow -> RegTL
                                                        |
                                    spill transfers / final scheduling
                                                        |
                                 MachineIR / Bin2Bin / object emission
```

Its subsystems have separate roles:

- **Metacode / Infobank:** parse and normalize source-located machine metadata.
- **Unisel / Limeburg:** graph/constraint selection and BURS tree selection.
- **Schedrow:** scheduling dependencies, resources, timing, and algorithms.
- **RegTL:** allocation, liveness, interference, spills, and transfers.
- **Tunah:** bounded equality saturation through explicit semantic adapters.
- **Bin2Bin:** decoding, lifting, rewriting, encoding, translation, and caching.
- **MachineIR:** machine-level representation, including a D package and C++/D
  exchange interfaces.
- **TraceML:** metatracing frontend and lowering.
- **Exolayer:** callback, extension, and native FFI boundary (`exl_` C symbols).
- **VMWeave:** Lua VM descriptions compiled into embeddable runtimes.

See the [Limestone architecture chapter](../limestone/manual/01-framework-and-architecture.md)
for the representation-level contracts.

## Important boundaries

**VM execution is not native execution.** Firestone's stack and register machines
execute checked bytecode. A VM address is not a host pointer. Flintstone's
portable output is FSVM bytecode; native emission needs a target adapter.

**Instruction metadata is not an ABI.** Register names and opcode forms do not
by themselves specify calling conventions, stack frames, spill sequences,
relocations, or executable-memory ownership.

**Bindings are not all implemented language backends.** XBind lists D, Go, Java,
Lua, NodeJS, OCaml, Python, Ruby, and Rust. Only its Lua emitter is implemented;
other targets are inventories. Sandstone's SWIG bindings and Limestone's SWIG
Python bindings are separate mechanisms.

**Retired code is not a dependency fallback.** Yellowstone lives under
`.decomissioned/` and is not part of the default build. Tourchstone native
loading/invocation needs active Yellowstone qBFD and defaults off without it.
The build does not move retired sources back into the active tree.

Next: [Building and installing](02-building.md).
