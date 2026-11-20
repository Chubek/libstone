# IR generation

`archon irgen` turns one or more validated ArchDBs into a deterministic
directory of standalone C99 sources. The selected representation always emits
`IR.c` and `IR.h`; installed pipeline stages are then run in stable order and
may add support files such as graph and rewrite runtimes. Full generation emits
`ArchonEmbed.h`/`ArchonEmbed.c` as the stable, bounded C11 application facade;
it delegates to the generated component APIs and does not require `Driver.c`.
See [the pipeline guide](IRPipeline.md#c11-embedding-api).

## CLI

Generate Gated SSA for explicit databases:

```sh
python3 -m archon compile stdspec/riscv/rv32i.arch -o rv32.archdb
python3 -m archon compile stdspec/wasm/root.arch -o wasm.archdb
python3 -m archon irgen gated rv32.archdb wasm.archdb -o generated/gated
```

`-d/--database/--archdb ARCHDB` is also repeatable and may be combined with
positional ArchDBs. With no ArchDB arguments, Archon discovers and compiles every
`architecture` entry point below `stdspec/`.

**Target coverage is profile-specific.** Every
[bundled standard specification](stdspec/README.md) can be selected for classic
IR generation, including from a mixed set of databases. This describes
generation of the IR and target artifacts, not support for every operation,
assembly dialect, or ABI. A selected target can reject a program at instruction
selection, register allocation, or final emission. In particular, the stack VMs
do not have a physical register ABI, and admission to the classic profile does
not admit a target to the separate native MachineIR profile.

Generation includes `Oracle.c` and `Oracle.h` by default. They provide the
bounded shared-memory/IPC bridge used to ask an external process to transform
or annotate `.i` artifacts. Disable this optional integration with
`--no-oracle` or `--option oracle=false`. No IPC occurs merely because the
files were generated: a runtime Oracle socket must also be configured. See
[Oracle.md](Oracle.md) for the generated client/server API, protocol limits,
typed sidecars, and stage advice keys.

The supported representation names are:

* `gated`
* `memory-ssa`
* `sea-of-nodes`
* `anf`
* `cps`
* `typed-tac`
* `untyped-tac`
* `supercombinators`

Backends may also publish aliases. Pass backend or stage options with
`--option NAME=JSON`; values are parsed as JSON and appear in
`context["options"]`. The output is built in a sibling temporary directory and
renamed into place. A byte-identical existing tree is retained. Replacing any
other nonempty tree requires `--force`. ArchDB inputs are protected even when
forced: an output directory may not equal or contain an input path.

When more than one ArchDB is selected, shared target facts, rewrite banks, and
instruction-selection patterns contain all of them. The final linked pipeline,
allocator, and emitter use the lexicographically first architecture unless one
is chosen explicitly:

```sh
python3 -m archon irgen anf rv32.archdb rv64.archdb \
  --target RISCV64 -o generated/rv64
```

`--option target='"RISCV64"'` is the equivalent generic-option spelling.

Full generation also includes `Assembler.sh`, `Linker.sh`, `Proof.py`,
`Proofs.smt`, `PrettyPrint.py`, `Translate.py`, and `RunSandbox.c`. The shell
wrappers have executable permissions when written by `archon-irgen`. See
[IRTools.md](IRTools.md) for provider selection and the generated tools' input
and output formats. The sandbox runner is a separate Linux executable, not
part of the portable C99 IR libraries or a stage that runs at generation time.

## Python API and plug-in contract

New callers can use `archon.irgen.api`; the original `archon.irgen` API
shown below remains available and exposes plug-in modules for callers that
need them. See [Embedding.md](Embedding.md) for both library languages.

```python
from archon.irgen import generate_ir, write_generated_directory

files = generate_ir(
    "gated",
    [rv32_db, wasm_db],
    options={"oracle": False},
)
write_generated_directory(
    files,
    "generated/gated",
    input_paths=["rv32.archdb", "wasm.archdb"],
)
```

The main API is:

```python
build_context(ir, architectures, *, options=None) -> dict
generate_ir(ir, architectures, *, options=None, run_stages=True) -> dict[str, str]
write_generated_directory(files, destination, *, input_paths=(), force=False) -> Path
available_representations() -> tuple[str, ...]
pipeline_stages() -> tuple[module, ...]
standard_spec_entry_points(root=None) -> tuple[Path, ...]
compile_standard_architectures(root=None) -> list[dict]
```

`generate` aliases `generate_ir`, and `write_directory` aliases
`write_generated_directory`.

Every representation module below `archon.irgen` has this interface:

```python
NAME = "canonical-name"
ALIASES = ("optional-alias",)

def emit_ir(context: dict) -> dict[str, str]:
    return {"IR.c": source, "IR.h": header}
```

Every pipeline-stage module exposes:

```python
def emit(context: dict) -> dict[str, str]:
    return {"Additional.c": source, "Additional.h": header}
```

Stages may define numeric `STAGE_ORDER` (or `ORDER`); otherwise they run at
order 1000, with module names breaking ties. Registry discovery happens for
each generation, so optional modules added after the package was imported are
still found. A representation module is not also treated as a stage if it
provides both entry points.

The context is plain JSON-compatible data:

```json
{
  "ir": "gated",
  "architectures": ["validated ArchDB objects"],
  "architecture_names": ["RISCV32"],
  "options": {"oracle": false}
}
```

ArchDBs are validated and deep-copied at this boundary. Emitters return text;
they do not write files. Output names must be safe relative POSIX paths—absolute
paths, `..`, symlinks, and file/directory collisions are rejected.
For compatibility with independently distributed stages, invocation contexts
also carry `databases`/`archdbs` list aliases and `database`/`db` aliases for
the first target. New modules should use `architectures`.

## Generated Gated SSA API

The Gated backend provides:

* typed `bits`, `uint`, and `sint` values up to 4096 bits;
* distinct effect-token values;
* named basic blocks and block sealing;
* parameter, constant, generic, target-instruction, phi, gate, effect-start,
  and effect nodes;
* checked builders with sticky diagnostics;
* a structural verifier;
* a human-readable printer and a `Program.i` X-macro printer;
* target-opcode and architecture metadata derived from all selected ArchDBs.

`IR.h` documents every declaration. A minimal builder looks like:

```c
struct archon_ir_program program;
struct archon_ir_builder builder;
archon_ir_block_id entry;
struct archon_ir_value x, zero;

archon_ir_program_init(&program);
archon_ir_builder_init(&builder, &program);
entry = archon_ir_create_block(&program, "entry");
archon_ir_position_at_end(&builder, entry);
x = archon_ir_build_parameter(&builder, archon_ir_uint(32), 0);
zero = archon_ir_build_constant(&builder, archon_ir_uint(32), 0);
/* Build target, phi, gate, and effect nodes here. */
archon_ir_program_dispose(&program);
```

Compile generated sources without the Python runtime:

```sh
cc -std=c99 -Wall -Wextra -Werror -pedantic \
  generated/gated/IR.c application.c -o application
```

### `Program.i`

`archon_ir_emit_program_i(stream, program)` emits these macro records:

```c
ARCHON_IR_PROGRAM_BEGIN(version, "gated")
ARCHON_IR_BLOCK(id, sealed, "name")
ARCHON_IR_NODE(id, block, opcode, type_kind, width, payload)
ARCHON_IR_INPUT(node_id, value_id, incoming_block_or_zero)
ARCHON_IR_PROGRAM_END()
```

Define the five macros and include `Program.i` to consume it in any C99 tool.
The generated runtime also supplies `archon_ir_load_program_i`. Compile `IR.c`
with the macro below to activate that loader:

```sh
cc -std=c99 '-DARCHON_IR_PROGRAM_FILE="Program.i"' \
  generated/gated/IR.c consumer.c -o consumer
```

Without `ARCHON_IR_PROGRAM_FILE`, the loader returns
`ARCHON_IR_PROGRAM_UNAVAILABLE`. Loading uses the same verifier as programmatic
construction.

This representation-specific input is distinct from the canonical Macro ABI
v1 Program contract. Every generated `Pipeline.h` exposes
`archon_pipeline_emit_program`, which decodes and verifies the native input and
writes canonical `ARCHON_PROGRAM_*` records to a caller-selected stream without
modifying the source `Program.i`.

## Native runtime profile

`--profile native` adds a standalone C99 backend library and runtime driver
for a reviewed ArchDB. Optional `--abi` must match its profile (`LP64` for
RV64I/RV64IM, `SysVAMD64` for AMD64, `AAPCS64` for AArch64). It does not
replace the eight generated representations or change the default legacy
pipeline. See [Embedding.md](Embedding.md) for the build, [MachineIR.md](MachineIR.md)
for the runtime input contract, and [NativeScalarGAS.md](NativeScalarGAS.md)
for AMD64/AArch64 capabilities.
