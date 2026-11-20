# Archon

Archon compiles `.arch` architecture descriptions into an Architecture Semantic
IR (ASIR) database. Its Python runtime uses that database to assemble,
disassemble, and execute instructions without ISA-specific code in the runtime.

## Quick start

Use Python 3.10 or newer; there are no third-party dependencies or installation
steps. Run these commands from `cornerstone/Archon` in the repository:

```sh
python3 -m archon check stdspec/riscv/rv32i.arch
python3 -m archon compile stdspec/riscv/rv32i.arch -o rv32i.archdb
python3 -m archon inspect rv32i.archdb
python3 -m archon asm rv32i.archdb 'add x1, x2, x3'
# b3003100
python3 -m archon dis rv32i.archdb b3003100
python3 -m archon run rv32i.archdb 'add x1, x2, x3' --reg x2=10 --reg x3=20
# JSON register file with x1 = 30
```

`tools/archonc` is a standalone wrapper for `python3 -m archon compile` that
can be invoked by path from any working directory. `compile` writes a
schema-versioned UTF-8 JSON database, to stdout if `-o` is omitted. Object keys
are sorted and list order is preserved so the same compiled ASIR produces the
same bytes. Use `inspect --json` to see the entire database. If an ArchDB uses
an unsupported schema version, recompile the source.

`check` and `compile` accept repeatable `-D NAME=INTEGER` parameter overrides
and `--features=I,M` to replace the architecture's default enabled features.
Omit `--features` to keep the defaults, or pass `--features=` to enable none.
`asm` prints hex bytes in target byte order, or writes raw bytes with `-o PATH`
(`-o -` for stdout). `dis` accepts hex bytes or `--file PATH` (`--file -` for
stdin). `run` executes one assembly instruction against a zero-initialized
register file with optional `--reg NAME=VALUE` overrides; it prints the final
register file as JSON.

Assemble, disassemble, or run a whole file with local labels:

```sh
python3 -m archon compile examples/toy8.arch -o toy8.archdb
python3 -m archon asm-program toy8.archdb examples/toy8.s -o toy8.bin
python3 -m archon dis-program toy8.archdb toy8.bin
python3 -m archon run-program toy8.archdb examples/toy8.s
# r1 = 254, r2 = 6, r3 = 4
```

Program commands take an optional `--base ADDRESS` for byte layout; it doesn't
insert padding. Assembly and disassembly read stdin when their input path
is `-` and write to stdout by default. `asm-program` outputs raw bytes.
`run-program` executes in file order with no PC, memory, or branches.

Generate a standalone C99 encoder and decoder from the same database:

```sh
python3 -m archon gen rv32i.archdb -o rv32_codec.c --prefix rv32
cc -std=c99 -c rv32_codec.c -o rv32_codec.o
```

The generated code takes positional operands in declaration order and exposes
`rv32_encode` and `rv32_decode` in this example. Operands are `uint64_t`;
signed immediates use sign-extended bit patterns (for example, `UINT64_MAX`
represents `-1`). It handles binary encodings up to 64 bits, not assembly
syntax or instruction execution, and needs no Archon runtime.

Use `gen --emulator` to add native instruction execution to the generated
C99. See the [C99 emulator guide](Emulator.md) for the API and a runner that
executes the Toy8 example without Python.

Generate a standalone compiler IR runtime plus the common graph and rewrite
pipeline support:

```sh
python3 -m archon irgen gated rv32i.archdb -o generated/gated
cc -std=c99 -Wall -Wextra -Werror -pedantic \
    -c generated/gated/IR.c -o generated/gated/IR.o
```

Eight IR representations are available, and omitting ArchDB arguments compiles
all bundled standard-specification entry points. See the
[IR-generation guide](IRGeneration.md) for the CLI, Python plug-in contract,
atomic output rules, Gated SSA builder/verifier API, and `Program.i` macros;
the [pipeline guide](IRPipeline.md) and [artifact contracts](IRArtifacts.md)
cover linked stages, the generated `ArchonEmbed.h` C11 API, and versioned
interchange. The [Oracle guide](Oracle.md)
documents the optional local transformation protocol and typed annotations.

## Scope

This implementation covers fixed-width instruction encodings, architecture
parameters and features, optional register files, named scalar state, bounded
typed stacks, and bounded byte-addressed memory. ArchSem provides typed
bit-vector expressions, transactional stack and memory operations, and
sequential or simultaneous state updates. The supplied
[standard specifications](stdspec/README.md) cover selected RISC-V,
AArch64, MIPS32, AMD64, PDP-11, WebAssembly, JVM, and CLR instructions.
PDP-11 register-direct operations include condition-code effects; the virtual
machine targets execute only the stack operations whose behavior is faithfully
representable by the current straight-line semantics. None is a complete ISA
or virtual-machine implementation.
Whole-file assembly supports local labels in raw instruction streams;
object files, external relocations, and general program simulation are not
supported.

The root `AGENTS.md` (§32) documents the phased roadmap. IR generation now
provides standalone representation and pipeline libraries; later phases connect
their artifact schemas into a complete compiler, extend the frontend and
executable semantics, add ABI and linking support, and build proof, lifting, and
timing infrastructure. The syntax and examples in that design document are not
all accepted by the current frontend. See the
[implemented language subset](Language.md) for a complete example and its
rules.

Run the tests with:

```sh
python3 -m unittest discover -s tests -v
```
