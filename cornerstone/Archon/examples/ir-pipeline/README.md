# Generated IR pipeline example

This directory is a strict-C99 integration fixture for `tools/archon-irgen`.
It uses the bundled RV32I ArchDB and keeps every interchange file versioned and
human-readable.

From any working directory:

```sh
/path/to/Archon/examples/ir-pipeline/build.py demo
```

The script:

1. compiles `stdspec/riscv/rv32i.arch`;
2. invokes `tools/archon-irgen anf ...`;
3. compiles every generated `.c` file with
   `-std=c99 -pedantic-errors -Wall -Wextra -Werror`;
4. links the generated Oracle IPC runtime (it remains inactive when no Oracle
   socket is configured);
5. checks the standalone scheduler, selector, and allocator contracts;
6. links and runs `embed_client.c` through `ArchonEmbed.h` without `Driver.c`;
7. links and runs the generated ANF CLI driver through register allocation;
8. checks exact RV32I GAS and Sandstone assembly.

`embed_client.c` initializes every option explicitly, supplies an inactive
`ArchonOracleOptions`, selects e-graph rewriting and GAS output, and receives a
fixed-capacity `ArchonEmbedResult`. It does not inspect arguments or the
environment.

Other useful commands are:

```sh
./build.py build
./build.py rebuild-check
./build.py clean
./build.py demo --build-dir "$PWD/build with spaces"
```

`clean` removes only files recorded by this example's manifest and then removes
empty directories. It does not recursively erase an unrecognized build tree;
an unrelated file placed in the build directory survives. `build` also refuses
to adopt a nonempty directory that has no example manifest.

## Fixture layout

- `fixtures/v1/*/Program.i` contains small input programs for ANF, untyped TAC,
  and Sea-of-Nodes. The directory version is part of the fixture contract.
- `expected/v1/scheduler` is the scheduler's MachineIR v2 stream.
- `expected/v1/selector` is the selector's current compact MIR input and
  selected-instruction output.
- `expected/v1/regalloc` contains textual schedule, liveness, register-map, and
  exact allocation-report examples.
- `expected/v1/emitter/rv32i` contains a minimal executable emission section
  and the exact GAS/Sandstone output produced by the full ANF demonstration.

These groups are deliberately separate. They document current stage contracts;
the basename alone never makes two artifacts interchangeable. See
[`../../IRArtifacts.md`](../../IRArtifacts.md) for the artifact map, inclusion
rules, generated-driver controls, and current integration limits.
