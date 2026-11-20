# Backend implementation and boundaries

Archon now has two generator paths. The legacy path continues to generate eight
representations and the linked X-macro toolchain. The opt-in native path
consumes runtime generic MachineIR and emits static RV64IM/LP64 assembly. Its C
interface does not depend on a particular generated source representation.

The native path verifies the whole module before lowering it. The default
keeps the input order; optional pure-region scheduling checks its permutation
and def-use ordering before allocation. Allocation assigns block-local
registers and stack homes; independent checks validate the homes and frame
before ABI and CFG lowering emit physical instructions.
Selection uses explicit RV64IM mappings, with synthesized compare and trap
sequences for checked operations rather than assigning wrapping instructions
to trapping generic operations. It does not run the legacy e-graph selector.
The bounded assembly sink rejects invalid operands and only publishes on
success. A separate inspection mode records physical operands and symbolic
transfers without parsing generated assembly. A raw encoding mode handles
one function with no imports or globals; external relocation and ELF output
still use the configured tools. See [MachineIR.md](MachineIR.md),
[Embedding.md](Embedding.md), and the [capability matrix](CAPABILITY_MATRIX.json).

## Schema and retargeting

ArchDB v1 accepts optional independently versioned `abi_facts` and
`abi_facts_v2` sections; old records and existing Macro ABI contracts remain
readable. The v2 metadata admits separate stack-pointer state and stack return
locations where needed by the standard specifications.
The RV64IM provider generates its ABI register and alignment tables from those
facts. It matches the reviewed ISA fingerprint, checks the declared ABI
against the supported scalar LP64 convention, and verifies ArchSem expressions
where expressible. It binds the remaining forms to a reviewed target
manifest. The manifest labels
which instructions have ArchDB encodings rather than treating provider
encodings as ArchSem. Changing semantics, encodings, or ABI-relevant register
facts does not silently select the same provider.

The runtime text schema and backend API are versioned independently of
ArchDB. ABI facts are also emitted in target facts for non-native consumers.
Other standard targets remain codec/emulator profiles even when they provide
ABI facts. Absent facts remain unknown; do not synthesize ABI facts or proof
claims during migration.

## Integration boundaries

- Sandstone's C assembler consumes SymAS and produces a flat image or its JSON
  object format. An ArchDB is not a SymAS bank. The existing Sandstone dialect
  path remains in the legacy emitter. Native ELF assembly is independently
  consumable without converting or introducing a new SymAS bank.
- Yellowstone qBFD can inspect the emitted ELF and its relocations, but
  does not write these relocatable objects. The Yellowstone native linker
  does not apply the RV64 instruction-field relocations needed by this path.
  The configured assembler and LLD bridges produce static ELF artifacts.
  QDWARF and QWASM inspection do not implement native debug writing.
- Flintstone provides checked parse/lower/interpret APIs, Blackstone exposes
  host-context Lua optimization kernels, and Novostone Datalog evaluates
  finite typed facts. None defines the runtime MachineIR ABI. No native
  runtime depends on these components. Satie and Equinox-NG could be optional
  solver and optimization providers; their C++ interfaces are not part of the
  generated C99 core.

## Next milestones

1. Add editable selected and allocated MachineIR views beyond the current
   inspection stream. Extend scheduling with resource usage and real latency
   facts.
2. Extend the checked Gated SSA and Typed TAC adapters beyond their exact
   scalar subsets, and add runtime adapters for the other six representations.
3. Generalize the single-target provider with declarative ABI classification,
   split encodings, layout, and lowering records. ABI v2 standard-spec
   metadata does not yet provide executable call lowering for other ISAs.
4. Add direct Yellowstone object/link bridges only when the necessary
   relocation records, PC-relative pairs and range checks are supported.
5. Extend semantic evidence and trusted transformation publication before
   enabling native rewrite or Oracle proposals.
