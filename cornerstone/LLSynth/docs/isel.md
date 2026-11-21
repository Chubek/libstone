# Instruction selection with `std.ISel`

An `.isel` block builds an LLSynth program backend from a Sandstone ArchML
profile. Use it after lowering to an IR with **explicit register numbers and
immediates**. The result is Sandstone assembly using the selected concrete form
IDs, ready for `sandstone-as --archml`.

In the [runnable example](../examples/isel/select.llsynth), the IR operation
is `.op Load(destination: Int, offset: Int, base: Int): () -> ()`. These three
arguments are encoding operands; the operation has no implicit stack effects.

```text
.import "std.ISel"
.isel rv for Machine from riscv64 features base
.select Load(rd, imm, rs1) => load i64
.end
```

Run from the repository root:

```sh
perl cornerstone/LLSynth/scripts/llsynth.pl --mode=compile --program-target=rv \
  -o select.py cornerstone/LLSynth/examples/isel/select.llsynth
printf '1 16 2\n' | python3 select.py --compile > load.s
cat load.s
# .insn ld, 1, 2, 16
sandstone-as --archml sandstone/archml/stdspec/riscv64.aml -o load.bin load.s
od -An -tx1 load.bin
# 83 30 01 01
```

The example also defines backends `a64` and `x64`. With the same input,
`--compile --target a64` emits `.insn ldr64, 1, 2, 16`; `--target x64`
emits `.insn load64-base-disp32, 1, 2, 16`. Use the corresponding
`arm64.aml` or `amd64.aml` when assembling.

## Block syntax and selection contract

```text
.isel OUTPUT_TARGET for IR from PROFILE [features FEATURE,...]
.select LOCAL_OPCODE(FIELD,...) => ARCHML_IR_OP ARCHML_TYPE
.end
```

* Import `std.ISel` **before** the block in each source module using it. Plain
  and qualified imports both enable the directive in that unit. Indirect
  imports do not leak directives. The library needs Perl only at generation.
* `PROFILE` is the stem of a file in [`sandstone/archml/stdspec`](../../../sandstone/archml/stdspec)
  (`.aml`, then `.am`). Alternatively, use `from "path/to/profile.aml"`,
  relative to the block's file, including outside a repository checkout.
  The profile and provider participate in manifests and depfiles; generated
  tools do not read them again. As shipped, `amd64`, `arm64`, and `riscv64`
  have `selection` records. Other profiles may have encodings but no selector.
* `LOCAL_OPCODE` is an existing operation of `IR`. For each positional IR
  parameter give the corresponding *ArchML instruction operand name*, not the
  spelling of the IR parameter. Each must occur exactly once. For `Load`
  above, the third parameter is `rs1`, and ArchML declares `ld`'s operands
  as `rd, rs1, imm`. This is why the displacement moves to the end. IR
  parameters must have normalized type `Int`. Cover every operation in the
  backend's IR. Program target names must be unique.
* Matching uses `selection.pattern (ir-op OP TYPE)` and its `ref type`.
  Selection follows `ref instruction` to an encodable instruction; it does
  not guess a form by mnemonic. Eligible forms must have the requested
  operand set and all `instruction.requires` features enabled. Lowest
  nonnegative integer `cost` wins; ties use the selection record name in
  lexical order. No match is a generation error at the `.select` line.
* The feature list replaces `target.default-features`. Unknown features or
  missing prerequisites fail generation. The block consumes already legalized
  operands: callers must handle mode, implicit uses/defs, register allocation,
  ties (e.g. AMD64 two-address `add64-rr`), flag liveness, and prose constraints.
  ArchML's encoder checks the concrete register/immediate bounds and scales.
  Displacements are in bytes, and `.insn` accepts constant operands.

The same example works with `-T c11` instead of Python. The [integration
tests](../tests/stdlib/test_isel.py) check both generated tools. Set
`LLSYNTH_ARCHML_AS` to an ArchML-enabled `sandstone-as` to also check machine
bytes for all three targets.
