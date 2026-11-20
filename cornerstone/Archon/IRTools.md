# Generated IR tools

Full `archon-irgen` runs also emit two shell drivers, three Python tools,
and the source for a Linux runner.
They are separate from the generated C libraries: generation does not invoke
an assembler, linker, solver, or parser toolchain.

```sh
./tools/archonc stdspec/riscv/rv64im.arch -o rv64im.archdb
./tools/archon-irgen gated rv64im.archdb --profile native --no-oracle \
    --assembler llvm --linker lld -o generated
```

## Assembling and linking

```sh
generated/Assembler.sh -o module.o -- module.s
generated/Linker.sh -o program.elf --entry _start -- startup.o module.o
```

Choose the assembler with `--assembler gas|nasm|fasm|llvm|sandstone|archon`.
The default is GAS. `llvm` uses `llvm-mc`; `--assembler-program` selects a
specific executable, such as a cross assembler. The assembler must understand
the source's dialect. The wrapper does not translate GAS to Intel syntax or
make an unsupported legacy emitter target executable.

| Provider | Output and options |
| --- | --- |
| `gas` | Relocatable object from the configured GNU assembler. Known cross targets use a target-specific executable and flags. |
| `llvm` | Object from `llvm-mc`. Override its target with `--llvm-triple` and its attributes with `--llvm-mattr=+FEATURE`. |
| `nasm` | Select an object or flat format with `--assembler-format`; the AMD64 default is `elf64`. |
| `fasm` | The assembly source's `format` directive selects the format; the wrapper does not guess. |
| `sandstone` | `bin` (default) or Sandstone `obj` JSON. Supply `--sandstone-bank PATH`, `--sandstone-archml PATH`, or both. `--sandstone-syntax NAME` selects a syntax in the bank. |
| `archon` | Flat binary from the selected ArchDB's fixed-width instruction subset. It recognizes that database's canonical syntax and file-local labels, not GAS sections, assembler macros, or external references. |

The Sandstone choice also emits and registers `AsmOPT.lua`. It runs after
Sandstone expands source macros, includes, and Lua directives, before
instruction layout. With an RV64 target and `--sandstone-syntax default`,
it rewrites an exact `addi x0, x0, 0` line into `nop` only if the loaded bank
encodes both as the same four bytes without relocations. Other cases leave the
source unchanged. The rule preserves instruction bytes and label offsets.

Choose the linker with `--linker gnu|gold|mold|lld|yld`. The default is GNU
`ld.bfd`; `yld` runs Yellowstone's `qobjld`. Use `--linker-program` for an
explicit executable (for example, a newly built `qobjld`). `--linker-script`
sets the default layout file; `Linker.sh --script PATH` overrides it for one
run. A GNU-style script (`-T`) is interpreted by the selected linker. A
Yellowstone layout Lua file (`.lua`) requires `yld` and is not executed by
the shell. A bare tool name is looked up on `PATH`; relative input, output,
configured tool, and layout paths are relative to the caller's working
directory. These drivers assume trusted tools and trusted build scripts.
They stage the requested output before replacing the destination; they do not
turn external tools into a sandbox. Linking standalone ELF inputs also uses
`readelf` to identify GCC LTO objects.

The format produced by the assembler must be accepted by the linker. GAS,
LLVM, and NASM can produce ELF objects for supported targets. Sandstone's
flat binary and custom JSON are not ELF and are not directly linkable by
Yellowstone. The ArchDB-derived assembler also emits a flat binary, not a
relocatable object. These wrappers do not replace the stricter [native RV64
ELF bridge](BinaryEmission.md#producing-an-elf-with-external-tools).

### Link-time optimization

LTO is off by default. `--lto` selects an optimization path for the chosen
linker, and `--no-lto` explicitly selects the non-optimizing path.

- LLVM LLD enables its native LLVM bitcode optimization path. Supply at
  least one standalone LLVM bitcode input (for example, from `clang -flto`).
  With LTO off the wrapper requests `--lto-O0 --lto-CGO0`; LLD may still
  have to compile bitcode to finish a link.
- GNU ld, Gold, and Mold LTO use GCC's linker plugin through the GCC driver
  (`--lto-driver` overrides `gcc`). Supply at least one standalone GCC
  `-flto` object; the selected GCC and linker must support that plugin.
- Only Yellowstone plus `--lto` emits `LTO.lua`. The wrapper passes its
  adjacent file to `qobjld --lto-script`, separately from any layout script.
  This policy requests section reachability optimization in a supported
  native static ELF link. It is not an LLVM/GCC bitcode implementation.

An assembler does not produce compiler IR. To use an LLVM or GCC LTO route,
compile the relevant inputs with the corresponding compiler and pass them
to the linker driver rather than trying to reconstruct IR from assembly.

For example, with a built `qobjld` and native ELF objects:

```sh
./tools/archonc stdspec/amd64/root.arch -o amd64.archdb
./tools/archon-irgen anf amd64.archdb --linker yld --linker-program /path/to/qobjld \
    --lto --linker-script layout.lua -o generated-yld
generated-yld/Linker.sh -o app -- startup.o helpers.o
```

The default `LTO.lua` lets `qobjld` remove unreachable input sections.
The entry section and everything it references through relocations are
retained. Use linker `KEEP` selectors or list extra named roots in the Lua
policy's `keep` array for references the linker cannot infer. This is most
useful when each function or object has its own section. See
[Yellowstone's hook contract](../../yellowstone/ld/README.md#native-elf-whole-link-optimization-hook)
for supported relocations, layout features, and limits.

## Checking rewrite obligations

```sh
python3 generated/Proof.py --list-rewrites
python3 generated/Proof.py --verify all
python3 generated/Proof.py --verify REWRITE_ID initial.json rewritten.json
python3 generated/Proof.py --find-error --rewrite REWRITE_ID --programs initial.json
python3 generated/Proof.py --run MyProof.smt
```

`Proof.py` uses a bounded Python port of Satie's DPLL core, not a library or
external solver. `--run` solves one standalone SMT-LIB2 query in its
Boolean and fixed-width bit-vector subset (at most 64-bit vectors). A SAT
result includes a model. The driver looks beside itself for `Proofs.smt`;
use `--proofs PATH` if you rename or move that matching corpus.

The corpus contains the generated pattern-bank rules and selection patterns
with their value equations where modeled. These are static obligations,
not evidence that a rule was applied to a program. A modeled obligation is
proved only when the solver finds that unequal values are unsatisfiable.
An unmodeled rule or resource cutoff is `unknown`; any unknown prevents
`--verify all` from reporting full success. An explicit target selection
does not by itself claim an equivalence. Generated `Rewrite.h` also exposes
an opt-in observer whose callback reports stable rule IDs when the engine
applies a rule. An e-graph application can add an alternative without
changing the extracted program; these observations are not certificates.

The per-rule commands take pure value terms, not whole `Program.i`, MIR,
or executable programs. For example, a generated 32-bit RISC-V identity
may take this `initial.json`:

```json
{"target":"RISCV32","width":32,"inputs":["x"],"result":["add","x",0]}
```

In `rewritten.json` use the same fields with `"result":"x"` and select the
corresponding add-zero rule ID from `--list-rewrites`. `--find-error` checks
matching one-step instances (`--rewrite all` searches the inventory). This
does not discharge memory effects, traps, the C compiler's correctness, or
transformations outside the value fragment.

## Pretty-printing macro streams

```sh
python3 generated/PrettyPrint.py --format latex --out Program.tex Program.i
python3 generated/PrettyPrint.py --format troff --template listing.troff \
    --out Program.troff Program.i
```

The printer reads `.i` as text, not as C to be run or preprocessed. It
formats macro records, display-escapes their contents, and does not resolve
`#include` or choose conditional branches. It does not run LaTeX or troff.
Custom templates are trusted document source. Place one `{{body}}` on its
own line; an optional `{{title}}` inserts escaped inline text. Without
`--out`, output goes to stdout. See `--help` for syntax and size limits.

## Translating a source language

```sh
./tools/archon-irgen anf rv64im.archdb --no-oracle -o generated-anf
python3 generated-anf/Translate.py --grammar examples/translate/Lang.g \
    --absyn examples/translate/Lang.asdl --out MyProgram.i \
    examples/translate/MySource.lang
```

`Translate.py` embeds the canonical **Aurocks** and **ASDL** Perl source
files. It requires Python and Perl but no checkout or generated C compiler
invocation. It uses Aurocks grammar data and ASDL schema validation, then
lowers supported typed trees into the real ANF `Program.i` macro format.
The example declares its semantic mapping as JSON in ASDL comments between
`-- archon-translate-begin` and `-- archon-translate-end`. If there is no
inline mapping, the tool looks for `<grammar stem>.translate.json` beside the
grammar. `--mapping PATH` explicitly overrides either default.

Grammar and tree shape alone do not say what a language's operations mean.
Native grammar actions and schema code are not run. The current lowering
supports straight-line functions, typed parameters, local bindings, scalar
integer and Boolean expressions, and explicit returns. Other IR targets,
memory, effects, and unsupported grammar declarations fail explicitly.
See the [MiniCalc example](examples/translate/README.md) for the mapping
schema, grammar limits, and a C99 consumer that verifies the output.

## Running a linked program

`RunSandbox.c` is a separate runner for **Linux x86-64**. Give it a
freestanding, static non-PIE ELF executable with load segments below 4 GiB,
not a dynamically linked program, object file, `.i` stream, or raw
machine-code image. For example, link a freestanding `_start` that exits
directly instead of using libc startup. The runner does not emulate RV64 or
any other foreign target.

```sh
cc -std=c99 -O2 -Wall -Wextra -Werror -pedantic generated/RunSandbox.c -o run-sandbox
./run-sandbox --timeout-ms 2000 -- ./program.elf optional-argument
```

The launcher requires Linux 6.3+ with executable `memfd` support, `/proc`,
Landlock (ABI 3+), and seccomp filtering. It validates a sealed copy of the
executable, gives the child no stdin or inherited environment, and allows
only output and exit syscalls once the program is running. It fails closed
if the sandbox cannot be installed. Run it as a normal user.

The wall timeout defaults to 2,000 ms and accepts values from 1 to 30,000;
CPU and memory limits apply independently. The runner collects up to 64 KiB
of combined child stdout/stderr and reports status on its own stderr. It
returns the child's exit code except for reserved codes: `124` is wall
timeout, `125` is the output limit, and `126` is launcher failure. Child
signals and child exits that would collide with those codes return `1`, with
the actual result in the diagnostic.

The launcher limits the child, not the assembler, linker, or any other
tool that produced the ELF. Use a VM or stronger isolation for hostile
binaries; this runner shares the host kernel. Treat the child's output as
untrusted, including when consuming it in a terminal.

## Integration tests

Run `python3 -m unittest discover -s tests -p 'test_irgen_toolchain_integration.py'`
from `cornerstone/Archon`. To exercise the locally built downstream tools,
set `ARCHON_TEST_SANDSTONE_AS` to `sandstone-as`,
`ARCHON_TEST_SANDSTONE_BANK` to a compiled `riscv64.dsymspec` bank, and
`ARCHON_YLD` to `qobjld`. The RV64 execution test uses Unicorn with RISC-V
support.
