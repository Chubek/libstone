# 1. Architecture and scope

Flintstone is an owning, typed intermediate-language framework. A compiler can
retain domain operations, specialize type and dialect parameters, lower to
scalar SSA, optimize, execute portable bytecode, or hand off to a native backend.
Different levels may coexist during conversion. Unregistered operations are
never accepted as if their semantics were known.

```text
Text / C++ builder
        |
        v
Module + functions + typed SSA values + explicit CFG
        |
        +-- type/dialect/target specialization
        +-- structural, type, signature and dominance verification
        |
        v
High dialects -- conversion rules --> scalar core dialects
                                           |
                    +----------------------+-----------------------+
                    |                                              |
                    v                                              v
              Firestone SSA                            explicit target selector
                    |                                              |
              Blackstone kernels                          Limestone Schedrow
                    |                                              |
                    v                                       Limestone RegTL
              VM virtual ISA                                      |
                    |                                       Sandstone SymAS
              Firestone bytecode                                   |
                    |                                     bytes + relocations
              register / stack VM                              + symbols
```

## Responsibilities of the Stone libraries

| Library | Flintstone's use |
| --- | --- |
| Firestone | CFG, dominance and natural-loop algorithms; verified scalar SSA; bytecode compilation, linking, binary format and VM execution. |
| Blackstone | Existing Lua kernel catalog and transactional SSA rewrite adapter. Flintstone does not implement a competing optimization catalog. |
| Limestone | Schedrow scheduling/verification and RegTL CFG liveness, allocation and allocation verification. Existing representations cross the dedicated native-adapter boundary. |
| Sandstone | Authoritative SymAS target/register/form records, timing queries, encoding-language execution and relocation identities. |
| Tourchstone | XBind target discovery, binding-spec resolution, validated binding manifest and generated C signature checks. |

These integrations execute existing implementations. Standalone builds compile
the narrow Schedrow/RegTL source set from Limestone itself; they do not copy the
algorithms into Flintstone. A full Limestone build supplies its existing targets
instead. Standalone Flintstone does not need Tunah, Satie, LMDB, libffi or a D
compiler merely to schedule and allocate an already selected function.

Yellowstone is currently decommissioned in this checkout. Flintstone does not
resurrect it. Tourchstone's native loader/invocation facilities depend on its
qBFD target and are not used as a hidden fallback. This does not affect XBind,
portable execution, or the native encoding handoff.

## Owning model

`Module` owns its target, named type bindings, dialect parameter overrides,
attributes and functions. A `Function` owns parameters, its result type, blocks,
the entry-block ID and function attributes. Each `Block` owns operations. An
`Operation` owns:

- a dialect-qualified name;
- zero or more typed result values;
- value references or explicitly typed literals/symbols;
- successor block IDs;
- predecessor labels for phi operands;
- typed attributes and a source location.

IDs are function-local unsigned 32-bit identities. `UINT32_MAX` is reserved.
They are not instruction positions or pointer addresses. Text input permits
names, but canonical output uses numeric IDs. Multiple results are supported in
the extensible IL; scalar Firestone lowering requires at most one result after
explicit dialect conversion.

## Levels

`Level::High`, `Level::Core`, and `Level::Machine` are ordered conversion stages,
not optimizer quality settings:

- **High:** domain operations, including the supplied `math` dialect.
- **Core:** scalar operations in `arith`, `cf`, `func`, and `mem`.
- **Machine:** a target-defined machine representation. The builtin `vm` dialect
  is a portable virtual-machine ISA, not x86/AArch64 machine code.

A module's level states the highest abstraction still permitted. A high-level
module may contain core/VM operations; a core module cannot contain high
operations; a machine module cannot contain unconverted core operations. This
permits incremental conversion without treating unknown operations as legal.

The native adapter is a separate typed handoff, not a claim that renaming
`arith.add` to `vm.add` performs native instruction selection.

## Determinism and trust

Registries and attributes use ordered maps; source vectors and stable IDs
determine externally visible order. Conversion rules are tried in registration
order. Selected instruction identities and explicit block layouts determine
backend output. No pointer address is used as an ordering key.

Dialect verifiers, lowering callbacks, native selectors, custom proof callbacks
and Lua kernels are application code, not sandboxed input. Flintstone checks
their declared structural contracts but cannot prove arbitrary transformations
semantically equivalent. Native ISA/ABI correctness belongs to the target
adapter; scheduling, allocation, binding and encoding legality are checked at
their respective boundaries.

Next: [Build and first programs](02-build-and-first-programs.md).
