# 4. First programs and tool workflows

These examples assume the standalone builds from [Chapter 2](02-building.md).
In an aggregate build, component executables normally live under the matching
subdirectory of `build/`, rather than directly in the binary root.

```sh
mkdir -p /tmp/opencode
```

## Flintstone: transform and execute IL

The committed [affine example](../flintstone/examples/affine.flint) declares
parametric type `T`, specializes it to `i32`, scales its input by three, and
adds twelve:

```text
module affine level high {
  target "vm" { pointer_bits = 64; index_bits = 64; endian = "little"; }
  types { T = i32; }
  dialect arith { overflow = "modular"; }
  dialect math { factor = 3; }
  func @affine(%x:param<T>) -> param<T> {
    ^entry:
      %scaled:param<T> = math.scale(%x);
      %answer:param<T> = arith.add(%scaled, 12:param<T>);
      func.return(%answer);
  }
}
```

Check, lower, and execute it:

```sh
build-flintstone/flintstone --check flintstone/examples/affine.flint
build-flintstone/flintstone --lower core flintstone/examples/affine.flint
build-flintstone/flintstone --run affine --arg 10 flintstone/examples/affine.flint
# 42
```

Core lowering specializes the types and replaces `math.scale` with arithmetic.
VM machine lowering is selected with `--lower machine` for target `vm`.

Try a loop and explicit aggregate scalarization:

```sh
build-flintstone/flintstone --run sum --arg 10 flintstone/examples/sum.flint
# 45
build-flintstone/flintstone --scalarize --run dot --arg 6 flintstone/examples/vector.flint
# 36
```

Scalarization is an explicit pass; execution does not silently eliminate all
aggregate types. Use `--budget 1000` to bound bytecode instructions, not callback
wall-clock time. Exhaustion is a failure, not a partial successful answer.

Emit portable register-VM bytecode:

```sh
build-flintstone/flintstone --backend vm --emit /tmp/opencode/affine.fsvm \
  flintstone/examples/affine.flint
```

`--backend vm-stack` selects stack bytecode. Neither file is a native object.
When executing the output through Firestone directly, install Flintstone's
declared runtime intrinsics with `flintstone::bind_vm_runtime`; the convenience
`flintstone::execute` API does that automatically.

The [Flintstone manual](../flintstone/manual/README.md) covers the full language,
imports/standard library, pipeline APIs, native targets, and diagnostics.

## Blackstone: inspect the kernel catalog

```sh
build-blackstone/blackstone-kernels blackstone/kernels --ir ssa
build-blackstone/blackstone-kernels blackstone/kernels --phase constant_evaluation
```

The catalog tool validates metadata and lists kernel names, analyses, and
preservation contracts. The shipped catalog has 71 kernels: strength reduction
and 70 integer/SSA local optimizations. See the [kernel inventory](../blackstone/docs/kernels.md)
for exact matching and semantic restrictions.

Run a kernel through Flintstone's host adapter:

```sh
build-flintstone/flintstone --kernels blackstone/kernels --kernel add_zero \
  flintstone/examples/sum.flint
```

A successful invocation may make no change if no legal pattern matches. A
kernel name does not authorize transformations outside its declared arithmetic,
type, and analysis contract.

## Limestone: exercise backend stages

These commands require a successful **full** Limestone build with compatible
dependencies, not just Flintstone's narrow scheduling/allocation service build.

Evaluate a closed TraceML expression through the CLI:

```sh
printf '%s\n' '((lambda x (add x 2)) 40)' | build-limestone/limestone-cli
printf '%s\n' '(if (lt 1 2) (add 20 22) 0)' | build-limestone/limestone-cli --trace-execution
```

Default TraceML compilation evaluates a closed integer program and produces
portable machine IR. Trace execution retains executed arithmetic and guards;
neither mode alone establishes a native ABI backend.

Exercise individual textual stages with committed fixtures:

```sh
build-limestone/limestone-cli --select-burs limestone/tests/fixtures/selection.limeburg
build-limestone/limestone-cli --schedule-il limestone/tests/fixtures/scheduling.schedrow
build-limestone/limestone-cli --allocate-il --allocator color limestone/tests/fixtures/allocation.regtl
build-limestone/limestone-cli --isa limestone/metacode/infobank/isa/riscv64.isa
build-limestone/limestone-cli --help
```

Selection rules, scheduling dependencies, allocation constraints, and encoding
are separately verifiable artifacts. Start with the [Limestone workflow
chapter](../limestone/manual/03-first-programs-and-workflows.md) before composing
a native pipeline.

## Sandstone: encode and assemble

### Small ISA loader: no SymAS compiler required

The Python loader can encode an instruction directly from shipped `.isa`
metadata. Run from the Libstone root:

```sh
python3 - <<'PY'
import sys
sys.path.insert(0, "sandstone/isa")
from simple_isa import load_arch

rv = load_arch("riscv64", directory="sandstone/isa")
encoded = rv.encode("add", rd="a0", rs1="a1", rs2="a2")
assert encoded == bytes.fromhex("3385c500")
print(encoded.hex())
PY
```

This exercises a particular encoded form, not a whole assembler, linker, or
calling convention. The small `.isa` loader and SymAS databanks are distinct
interfaces.

### Compile a SymAS bank

Original `.dsymspec` inputs require stock Lua and an external `dsl.lua` runtime.
Once those are available, use an explicit compiler path so the driver does not
have to discover a standalone build:

```sh
python3 sandstone/scripts/spec-compiler.py \
  --compiler build-sandstone/compiler/sandstone-spec-compiler \
  --dsl-dir /path/to/dsl-runtime --validate \
  -o /tmp/opencode/x86.symas sandstone/compiler/stdspec/x86.dsymspec
```

Alternatively compile an existing JSON spec without Lua evaluation:

```sh
build-sandstone/compiler/sandstone-spec-compiler --validate \
  -o /tmp/opencode/custom.symas /path/to/custom.json
```

The [spec guide](../sandstone/compiler/stdspec/README.md) describes the JSON and
Lua-source relationship. SymAS's normative format is [SYMAS.md](../sandstone/SYMAS.md).

### Assemble source

Save this as `/tmp/opencode/prog.s`:

```asm
.text
.globl _start
_start:
    mov eax, ebx
    ret
```

Then assemble with the bank just built:

```sh
build-sandstone/assembler/sandstone-as --bank /tmp/opencode/x86.symas \
  --syntax intel -o /tmp/opencode/prog.bin /tmp/opencode/prog.s
```

Flat binary output requires locally resolved symbols. For external references,
use `-f obj -o /tmp/opencode/prog.obj.json`; this is Sandstone's JSON object
representation, **not** an ELF object file. Shared-byte opcode/immediate
relocations such as the shipped RISC-V branch forms also require the object
path instead of guessed bit-splicing. See [the object guide](../sandstone/docs/object.md).

## Tourchstone: validate and lower binding specs

With the standalone XBind build:

```sh
build-xbind/xbind --list-targets
build-xbind/xbind --check-spec hello tourchstone/xbind/examples/hello
build-xbind/xbind --dump-ir hello tourchstone/xbind/examples/hello \
  > /tmp/opencode/hello.json
build-xbind/xbind --emit-check hello tourchstone/xbind/examples/hello \
  > /tmp/opencode/hello-check.cpp
c++ -std=c++20 -fsyntax-only -Itourchstone/xbind/examples/hello \
  /tmp/opencode/hello-check.cpp
build-xbind/xbind --emit-lua hello tourchstone/xbind/examples/hello \
  > /tmp/opencode/hello-bind.cpp
```

Each step proves something different:

1. `--check-spec` executes and validates the manifest's structure.
2. `--dump-ir` serializes declarations and annotations; it is not a parsed header AST.
3. Compiling `--emit-check` output checks the declared C signatures against headers.
4. `--emit-lua` produces wrapper source. Building a loadable module still needs
   DomLuaBind's generated C API, the library implementation, and Lua.

The [XBind manual](../tourchstone/xbind/MANUAL.md#ir-signature-checks-and-lua-generation)
provides the complete compilation/loading sequence. Execute only trusted specs:
they are Lua code, not sandboxed data. Lua is the only implemented XBind emitter.

Next: [Embedding and API contracts](05-embedding.md).
