# IR generation and artifact contracts

`tools/archon-irgen` is the checkout-safe spelling of:

```sh
python3 -m archon irgen IR [ARCHDB ...] -o DIRECTORY
```

The wrapper resolves the repository from its own real path, not the caller's
working directory. Arguments remain separate process arguments, so spaces in a
working directory, ArchDB path, or output path need no wrapper-specific
escaping:

```sh
/checkout/cornerstone/Archon/tools/archonc \
  /checkout/cornerstone/Archon/stdspec/riscv/rv32i.arch \
  -o "/tmp/Archon demo/rv32i.archdb"

/checkout/cornerstone/Archon/tools/archon-irgen \
  anf "/tmp/Archon demo/rv32i.archdb" \
  -o "/tmp/Archon demo/generated"
```

Generation writes a sibling temporary tree and atomically installs it. An
identical existing tree is left untouched. Replacing a different nonempty tree
requires `--force`; input ArchDBs remain protected even with `--force`.
`--option NAME=JSON` values are parsed as data, not as shell fragments.

## Artifact flow

The generation-time flow is:

```text
.arch source ──archonc──> ArchDB
                            │
                            └──archon-irgen IR──> strict-C99 sources
                                                 + target tables and patterns
```

For every IR form, the generated `pipeline-driver` supplies a bounded runtime
bridge through a representation-specific checked adapter:

| Stage | Input used by the driver | Files written |
| --- | --- | --- |
| `decode` | native `Program.i` embedded when `IR.c` is compiled | canonical stream available through `archon_pipeline_emit_program` |
| `rewrite` | verified neutral view and generated rule bank | `Rewritten.i` engine-statistics/decision record |
| `schedule` | neutral lowering view | canonical `MIR.i` |
| `isel` | neutral view and generated e-graph pattern bank | `Selected.i`, canonical `InstrMap.i` |
| `isched` | in-memory selected operations | canonical `Scheduled.i`, `RegMap.i` |
| `regalloc` | in-memory schedule and values | canonical `Regallocated.i` |
| `emit` | the `REGALLOC_EMISSION_*` section | `Program.s` or `Program.asm` |

`Regallocated.i` contains both allocation facts and the executable emission
section. The emitter ignores allocation records it does not consume.

When the optional Oracle runtime is configured, any `.i` artifact can be copied
to a private POSIX shared-memory object and offered to an external worker over
local IPC. A successful worker response may atomically replace the artifact and
write a separate typed `ARCHON_ORACLE_*` annotation macro stream. Sidecars keep
analysis metadata out of the underlying artifact grammar. Missing Oracle
configuration is a no-op; malformed responses or illegal stage directives are
diagnosed rather than trusted.

The standalone tools expose related but distinct contracts:

| Component | Contract exercised by the versioned example |
| --- | --- |
| ANF scheduler | ANF program → scheduler MachineIR v2 |
| bounded selector | compact selector MIR, or scheduler MachineIR v2 → `InstrMap.i` |
| instruction scheduler | selected operation/resource facts → schedule and register map |
| linear-scan allocator | `Scheduled.i` + lifetime `MIR.i` + `RegMap.i` → allocation report |
| assembly emitter | `REGALLOC_EMISSION_*` records → assembly |

The driver uses checked in-memory adapters; it does not simply rename each
standalone tool's output and feed it to the next parser. A basename such as
`MIR.i` therefore does not identify a schema by itself. The fixtures under
`examples/ir-pipeline/expected/v1` remain separated by producer and consumer.

The generated [IR tools](IRTools.md) consume explicit paths. The shell
drivers run after assembly source has been produced; they are not C pipeline
passes and do not change any X-macro contract. Build `RunSandbox.c` separately
to execute a supported, already-linked native Linux ELF. It does not take IR,
assembly, or a raw function image and does not emulate a foreign target.

The allocator's canonical report includes `MacroABI.h` when used as a C
X-macro stream, so C consumers must provide that companion header. The assembly
emitter reads the report as checked text and does not invoke a preprocessor.

## X-macro inclusion

The source-side `Program.i` is an include fragment, not a standalone C
translation unit. Its schema is representation-specific because it constructs
the selected native IR directly.

| IR form | Inclusion control |
| --- | --- |
| ANF | `ARCHON_IR_PROGRAM_FILE`; legacy alias `ARCHON_PROGRAM_FILE` |
| untyped TAC | `ARCHON_IR_PROGRAM_FILE`; compatibility alias `ARCHON_IR_PROGRAM` |
| typed TAC | `ARCHON_IR_PROGRAM_FILE`; compatibility alias `ARCHON_IR_PROGRAM` |
| Memory SSA | `ARCHON_IR_PROGRAM_FILE`; compatibility alias `ARCHON_IR_PROGRAM` |
| Sea of Nodes | `ARCHON_IR_PROGRAM_FILE`, or `ARCHON_IR_INCLUDE_PROGRAM` for `"Program.i"` |
| CPS | `ARCHON_IR_PROGRAM_FILE`; compatibility alias `ARCHON_CPS_PROGRAM_FILE` |
| gated SSA | `ARCHON_IR_PROGRAM_FILE` |
| supercombinators | `ARCHON_IR_PROGRAM_FILE`; compatibility alias `ARCHON_SUPERCOMBINATOR_PROGRAM_FILE` |

Optional decoders remain available as library code when no file macro is
defined, but report that no embedded program is available.

The macro value must itself be a C string token:

```sh
cc -std=c99 -pedantic-errors \
  '-DARCHON_IR_PROGRAM_FILE="/absolute/path/to/Program.i"' \
  -c generated/IR.c
```

Quoted includes search the generated source's directory first. An absolute
macro path is the unambiguous choice when another artifact with the same
basename exists. Otherwise, keeping fixtures outside the generated tree and
adding their directory with `-I` avoids modifying the atomic output tree.

Macro punctuation is also schema-specific:

- the ANF, untyped-TAC, and Sea-of-Nodes v1 fixtures use statement-like calls
  with semicolons;
- compact selector `MIR.i` callbacks provide their own statement terminators,
  so its records omit semicolons;
- scheduler and allocator streams are textual macro records; their parsers
  validate the exact numeric and string token forms they document.

Never infer a schema from a filename or include two representation schemas into
one `IR.c`. `Rewrite.c` has its own optional
`ARCHON_REWRITE_PROGRAM_FILE`/`ARCHON_REWRITE_INCLUDE_PROGRAM` controls.

## Canonical neutral artifacts

After decoding and verifying the native source, `Pipeline.c` can normalize it
to the shared Macro ABI v1 contracts:

```c
int archon_pipeline_emit_program(
    FILE *output, char *diagnostic, size_t diagnostic_capacity);
int archon_pipeline_emit_mir(
    FILE *output, char *diagnostic, size_t diagnostic_capacity);
```

`archon_pipeline_emit_program` writes canonical `ARCHON_PROGRAM_*` records;
`archon_pipeline_emit_mir` writes canonical `ARCHON_MIR_*` records. Both APIs
decode afresh, produce deterministic stream IDs, return zero on success, and
never replace the representation-specific source file. The caller owns the
output stream and chooses its path, such as `CanonicalProgram.i`. Inputs whose
CFG or effects cannot be represented by the neutral lowering view are rejected
rather than silently linearized.

## Generated embedding facade

`ArchonEmbed.h` and `ArchonEmbed.c` wrap the existing pipeline and emitter APIs
for applications that do not want the generated CLI. `archon_embed_run` accepts
typed rewrite, dialect, stopping, output-path, hook, and artifact-policy
options. It returns an `ArchonEmbedResult` containing bounded diagnostics and
the exact reached/failed component status; it does not print diagnostics.

The facade removes stale fixed-name intermediates before each run.
`ARCHON_EMBED_ARTIFACTS_RETAIN` leaves newly produced files available for
inspection, while `ARCHON_EMBED_ARTIFACTS_DISCARD` removes them after success
or failure. Neither mode removes `Program.i` or successful assembly output.
Oracle-enabled output adds an explicit options pointer. Oracle-disabled output
contains no Oracle header/type/function reference and links without
`Oracle.c`. See [the embedding example](examples/ir-pipeline/embed_client.c).

## Generated driver: stopping, restarting, and cleanup

The generated driver recognizes:

```text
decode rewrite schedule isel isched regalloc emit
```

Stop after a stage with:

```sh
./pipeline-driver --stop-after schedule
```

A stopped run retains the artifacts it produced. A complete run removes
intermediate files unless `--keep-artifacts` is present:

```sh
./pipeline-driver --dialect gas --keep-artifacts
```

When no runtime `Program.i` exists and `Regallocated.i` already exists, a full
run deliberately restarts at the emitter. Remove that manifest-owned
`Regallocated.i` to rerun the embedded source program from `decode`.

`--list-artifacts` prints the fixed driver manifest:

```text
Program.i Rewritten.i MIR.i Selected.i InstrMap.i
Scheduled.i RegMap.i Regallocated.i Program.s Program.asm
```

`--clean` preserves `Program.i` because it is input and removes the other fixed
names. It does not recurse, expand globs, or remove a custom `--output` path.
Run it only in the intended artifact directory; a fixed artifact name may
otherwise refer to unrelated user data.

The example's `build.py clean` is stricter: it unlinks only paths in its JSON
manifest, rejects traversal and symlink escapes, and removes directories only
when they become empty. Unrelated files survive.

## GAS and Sandstone

Choose syntax at runtime:

```sh
./pipeline-driver --dialect gas
./pipeline-driver --dialect sandstone
```

GAS defaults to `Program.s`, starts with a `#` provenance comment and `.text`,
and uses validated canonical target templates.

Sandstone defaults to `Program.asm`. It starts with a SandAS Lua prelude that
checks the loaded SymAS target when target metadata is available, then emits
the same checked instruction templates. It is Sandstone assembly source, not a
GAS-compatible object file.

An architecture is supported only when its ArchDB provides a dialect table or
the emitter has a validated built-in profile. Current built-ins cover the
shipped RISC-V, AArch64, and MIPS profiles. Toy8 has no validated GAS or
Sandstone profile and reports `unsupported dialect` unless an embedding
supplies emitter hooks.

## Current limits

- The linked adapters currently lower straight-line pure integer operations or
  explicit target operations. CPS and supercombinator operations without a
  validated equivalence/target identity stop at instruction selection.
- Multi-block CFGs stop before selection unless control-flow lowering is
  represented explicitly; the pipeline never linearizes branches implicitly.
- The neutral rewrite bridge executes the selected directed or equality-
  saturation engine, but preserves its verified input and records unknown
  equivalence until ArchDB carries a proof-result fact.
- The driver's selector uses the generated bounded e-graph engine through its
  in-memory API. The same selector remains available as a standalone executable
  with its checked MIR decoder.
- Calls require ABI lowering. Register spills and tied operands require
  target-specific lowering before the executable emission section can be
  written.
- Selecting several ArchDBs builds shared fact and pattern banks, but one
  generation-time `--target` supplies the linked allocator/emitter. A generated
  binary does not switch target ABIs at runtime.
- The emitter writes assembly only. Object files, relocations, and linking are
  outside this demonstration.

At the Python orchestration boundary, every generation stage receives the
canonical JSON-compatible context:

```text
{"ir": ..., "architectures": [...], "architecture_names": [...], "options": ...}
```

Compatibility aliases are added only at plug-in invocation. A stage that
writes into the process working directory, returns paths instead of
`dict[str, str]`, or rejects the shared context is an integration error and is
reported rather than silently skipped.
## Native runtime interchange

The optional native backend accepts typed `ArchonMIRModule` views and the
independent `archon-mir 1` text grammar documented in [MachineIR.md](MachineIR.md).
The runtime never preprocesses its inputs. Native generation publishes
`BackendManifest.json`, with a validated target identity, ArchDB fingerprint,
ABI, and separately versioned public API and MachineIR schema. It does not
publish `MIR.i` or any of the legacy artifacts described above.

The `--emit-physical` driver option writes `archon-physical 1`, the target name
and ArchDB SHA-256, then functions, labels, physical instructions, symbolic
branch/call/address records and data definitions. Symbolic transfers are
retained until the assembler/linker resolves them; the stream ends with `end`.
This format is for inspection, not a compiler restart point.

`--emit-code` is an alternative output mode for a single function with no
imports or globals. It returns plain RV64IM instruction bytes starting at the
entry point, not a serialized MachineIR artifact or an ELF container. See
[BinaryEmission.md](BinaryEmission.md#raw-instruction-bytes).

The generated target facts carry declared ABI v1 or v2 as JSON. The optional
`ARCHON_TARGET_OPERAND_REGISTER_RANGE` callback reports explicitly bounded
register operands without changing the existing operand callback. The C
`archon_target_operand_register_range` query also supplies the effective
range for an unbounded register class.
