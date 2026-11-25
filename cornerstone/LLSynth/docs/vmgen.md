# Embedded VM targets

`.vmgen for IR` embeds a [VMWeave Lua specification](../../../limestone/vmweave/README.md)
in an LLSynth entry specification. The VM implements the lowered stack IR;
the LLSynth grammar and its `'lower` actions still define how a source program
becomes that IR.

```text
.ir Tape
  .op PUSH(value: Int): () -> (Int)
  .op MIX: (Int, Int) -> (Int)
.end

.vmgen for Tape
local v = require("vmweave")
return v.vm {
  name = "PairsVM",
  stack = { cell = "int64_t", capacity = 64 },
  components = { "insncode", "dispatch" },
  instructions = {
    { name = "PUSH", opcode = 7, operands = { "cell" }, abstract = "operand 0;" },
    { name = "MIX", opcode = 29, abstract = "add; const 2; mul;" },
  },
}
.end
```

Use one block per entry specification. Put `.end` on its own line; it always
ends the block, even inside a Lua long string or comment. Other lines are passed
through as Lua, not expanded as LLSynth includes or declarations. A textual
`.include` may contain an entire block; imported modules cannot declare one.
Keep the opening and closing directives in the same physical file.

## Build and run

Build or install Limestone's `vmweave-cli` with `--emit-c-bundle` support. The
LLSynth generator finds it on `PATH` or via `VMWEAVE_EXECUTABLE` (a single
executable path, not a shell command). From the repository root:

```sh
cmake -S limestone -B build/limestone
cmake --build build/limestone --target vmweave-cli
export VMWEAVE_EXECUTABLE="$PWD/build/limestone/vmweave/vmweave-cli" # adjust for your build
scripts/llsynth.pl --mode both --program-target vmweave -T python3 \
  -o pairs.py cornerstone/LLSynth/examples/vmgen/pairs.llsynth
printf '11 10\n' | python3 pairs.py --compile -o pairs.c
cc -std=c99 pairs.c -o pairs
./pairs    # 42
```

`--program-target vmweave` sets the generated compiler's default. When a different
default backend is available, `--compile --target vmweave` selects this backend
per invocation. `--check` validates the embedded specification and its IR
contract; it does not compile C instructions. `--interpret` still runs the
LLSynth semantics facet, not the embedded VM. Existing specifications without
`.vmgen` don't invoke VMWeave and retain their existing targets.

LLSynth calls `vmweave-cli --emit-c-bundle` once at generation time. The
versioned JSON bundle contains the runtime C and the validated instruction
contract from the same lowered VM. LLSynth records the VMWeave executable
in dependency manifests and depfiles. VMWeave evaluates Lua and can emit arbitrary
C supplied by the specification: build only specifications you trust. Lua should
not write to stdout, which is reserved for the bundle.

The generated Python and C/C++ LLSynth tools need no VMWeave or Lua installation.
They compile to a standalone **C99** program containing the generated runtime
and a decoded instruction tape. The C/C++ library API returns this source from
its normal compile entry point when `vmweave` is the default target. The
standalone runner initializes the VM, executes through the selected VM handlers
with a budget of one step per tape instruction, and prints the one signed
integer left on the stack. It reports VM status errors on stderr.

## Contract and limits

- A target needs `int64_t` cells, switch dispatch, and the `insncode` and
  `dispatch` components. The tape is generated directly, so VMWeave's textual
  tape compiler is not required.
- Each operation in the chosen IR maps to a VM instruction with the same
  case-sensitive mnemonic, operand and stack-effect counts. LLSynth immediate
  operands and stack values must be `Int`; VM instructions use `cell` operands
  and `next` flow. Extra VM instructions are allowed but not selected.
- LLSynth applies its existing rewrites, verifies the resulting tape against the
  IR and checks configured VM stack capacity and one final result. VMWeave
  remains authoritative for instruction behavior. Matching the interfaces does
  not prove an arbitrary C handler correct or equivalent to an LLSynth
  interpretation facet.
- This target does not generate custom binary bytecode or enable
  `--run-bytecode` for it. There is no lowering for branches, labels,
  calls, mixed IR tapes, non-integer cells, or other dispatch strategies.
  VMWeave native images/JIT and custom runtime initialization are not part of
  this integration.
