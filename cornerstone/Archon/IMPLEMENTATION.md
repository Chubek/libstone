# Backend implementation and boundaries

Archon now has two generator paths. The legacy path continues to generate eight
representations and the linked X-macro toolchain. The opt-in native path
consumes runtime generic MachineIR and emits assembly for RV64I/RV64IM LP64,
SysV AMD64 LP64, or AAPCS64 LP64. Its C interface does not depend on a particular
generated source representation.

The native path verifies the whole module before lowering it. On RV64, the default
keeps the input order; optional pure-region scheduling checks its permutation
and def-use ordering before allocation. Allocation assigns block-local
registers and stack homes; independent checks validate the homes and frame
before ABI and CFG lowering emit physical instructions.
Selection uses explicit RV64 mappings, with synthesized compare and trap
sequences for checked operations rather than assigning wrapping instructions
to trapping generic operations. RV64I expands modular multiply and rejects
divide, remainder and checked multiply where the implementation needs M.
It does not run the legacy e-graph selector.
The bounded assembly sink rejects invalid operands and only publishes on
success. A separate inspection mode records physical operands and symbolic
transfers without parsing generated assembly. A raw encoding mode handles
one function with no imports or globals; external relocation and ELF output
still use the configured tools. See [MachineIR.md](MachineIR.md),
[Embedding.md](Embedding.md), and the [capability matrix](CAPABILITY_MATRIX.json).

The [AMD64 and AArch64 providers](NativeScalarGAS.md) share the generic MIR
verifier and public C ABI. They use fixed stack homes and explicit target
instruction sequences to emit GAS, not the RV64 allocator or binary encoder.

## Schema and retargeting

ArchDB v1 accepts optional independently versioned `abi_facts` and
`abi_facts_v2` sections; old records and existing Macro ABI contracts remain
readable. The v2 metadata admits separate stack-pointer state and stack return
locations where needed by the standard specifications.
The RV64 provider generates its ABI register and alignment tables from the
qualified v1 facts. It matches the selected profile's exact ISA fingerprint, checks the declared ABI
against the supported scalar LP64 convention, and verifies ArchSem expressions
where expressible. It binds the remaining forms to a reviewed target
manifest. The manifest labels
which instructions have ArchDB encodings rather than treating provider
encodings as ArchSem. Changing semantics, encodings, or ABI-relevant register
facts does not silently select the same provider. The AMD64 and AArch64 GAS
providers independently validate their ABI v2 fields and exact full-DB pins.

The runtime text schema and backend API are versioned independently of
ArchDB. ABI facts are also emitted in target facts for non-native consumers.
Other standard targets do not gain native backends by declaring ABI facts.
See [native profile coverage](stdspec/riscv/README.md#rv64-native-profiles)
for remaining physical-target gaps. Absent facts remain unknown; do not
synthesize ABI facts or proof claims during migration.

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
3. Extend provider coverage with declarative ABI classification,
   split encodings, layout, and lowering records. ABI metadata alone does not
   enable native lowering for another ISA or ABI.
4. Add direct Yellowstone object/link bridges only when the necessary
   relocation records, PC-relative pairs and range checks are supported.
5. Extend semantic evidence and trusted transformation publication before
   enabling native rewrite or Oracle proposals.
