# Discovery integration slice

## Design before implementation

The acceptance boundary is a working linked and installed ExoML discovery SDK,
with independently exercised ucpp/CParser probes, LMDB persistence and the
MicroJS Lua contract. No new C declaration or ownership schema is introduced.

- Tests supply an explicit Clang resource directory; a sibling CMake directory's
  local variable is not a toolchain configuration. Missing resources retain the
  original ExoML error and cannot publish imports.
- CParser is a compatibility probe, not typed discovery. Capture its stdout
  diagnostics as well as stderr; report both successful and unsupported probes.
- CLI backend selection must support comparing linked discovery with the real
  `exoml-bind` subprocess using the same header, target and macro profile.
- A warm LMDB lookup still reparses with ExoML. Header, macro, provenance and
  compiler-profile changes must miss; invalid input must never use cached data.
- Permissions tests deny access to the database files themselves: making an
  existing directory read-only does not prevent writes to existing files.
- MicroJS tests inspect the canonical checkout's declarations, compile one
  generated implementation TU with both implementation switches, and exercise
  owned values, roots and callbacks. This is runtime evidence, not an ownership
  proof or a reviewed safe-export contract.

Regression cases: missing resources, original compiler diagnostics, unsupported
CParser C11 syntax, macro-dependent declarations, cold/warm/changed caches,
failed output publication, required-symbol rejection, and duplicate MicroJS
implementation linkage. Installed consumers use `Moonstone::Exotract` and
ExoML's actual `ExtractOptions`/`extract_c11`/`describe_translation_unit` APIs.

The linked SDK also provides contract validation directly through ExoML's
`ContractBundle` parser, validator and canonical serializer. Lua `Bind` may take
an optional `contract` sidecar path and attach its normalized contents to
`Internals.contract` only after validation succeeds. This checks ExoML's current
structural/semantic invariants; it does not authenticate reviews or prove exact
signature/ABI/ownership behavior. Neither annotations nor claimed review states
enable `reviewed_only` exports. Failed validation publishes no binder result.

Clang integration exposed premature special-member instantiation for ExoML's
recursive internal JSON object (`vector<pair<string, JsonValue>>`). The scoped
portability fix defines the value's special members after the type is complete;
it changes no schema. Regression evidence is the previously failing Clang
syntax compilation and the existing contract/CDecl round-trip tests. Exotract
test subprocesses now use the CMake-selected C compiler, including Lua and the
independent installed consumer, rather than silently using the host's `cc`.
