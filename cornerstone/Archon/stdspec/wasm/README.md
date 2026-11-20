# WebAssembly 1.0 (Wasm32) opcode subset

`root.arch` assembles and disassembles selected opcodes using standard,
case-sensitive WAT mnemonics. Each supported instruction is exactly one
byte and has no explicit immediate. Stack operands are implicit in Wasm,
not encoded in the byte or supplied in the Archon assembly string.

## Coverage

| Opcode bytes (hex) | Instructions | Count |
| --- | --- | ---: |
| `00`, `01`, `0f` | `unreachable`, `nop`, `return` | 3 |
| `1a`, `1b` | `drop`, untyped `select` | 2 |
| `45`–`4f` | `i32` zero test and comparisons | 11 |
| `50`–`5a` | `i64` zero test and comparisons | 11 |
| `5b`–`66` | `f32` and `f64` comparisons | 12 |
| `67`–`78` | `i32` bit counts, arithmetic, bitwise operators, shifts and rotates | 18 |
| `79`–`8a` | `i64` bit counts, arithmetic, bitwise operators, shifts and rotates | 18 |
| `8b`–`a6` | `f32` and `f64` unary and binary operators | 28 |
| `a7`–`bf` | Integer and floating-point conversions and reinterpretations | 25 |

The [golden vectors](encoding-golden.tsv) list all 128 mnemonics and bytes.
Wasm32 means 32-bit linear-memory addresses, not an absence of `i64` values.
No memory instructions are included here.

## Execution boundary

The architecture has a register-free `MachineState` with parallel bounded
stacks: a 64-bit value slot and an 8-bit type tag (`i32` = 1, `i64` = 2,
`f32` = 3, `f64` = 4).

| Opcode bytes | Executable | Encoding-only |
| --- | ---: | ---: |
| `00`, `01`, `0f` | `nop` (1) | `unreachable`, `return` (2) |
| `1a`, `1b` | `drop` (1) | `select` (1) |
| `45`–`bf` | 0 | All 123 numeric entries |
| **Total** | **2** | **126** |

`drop` removes one payload and its tag. Stack underflow is transactional:
neither stack changes if either pop fails. `unreachable` needs a trap,
`return` needs control flow, and `select` needs a conditional. Numeric
instructions require type-tag validation, and some additionally require
floating-point or trapping behavior. Current straight-line ArchSem provides
none of those facilities, so these entries deliberately have no executable
semantics.

From `cornerstone/Archon`:

```sh
python3 -m archon check stdspec/wasm/root.arch
python3 -m archon compile stdspec/wasm/root.arch -o wasm32.archdb
python3 -m archon asm wasm32.archdb 'i32.add'  # 6a
python3 -m archon dis wasm32.archdb 8a         # i64.rotr
python3 -m unittest discover -s stdspec/wasm -v
```

`asm-program` and `dis-program` can also process raw sequences of these
opcodes. Such a sequence is not a Wasm module, and Archon does not check
whether the sequence is valid on a Wasm operand stack.

The tests round-trip every golden vector through the serialized ArchDB, pin the
golden file hash, reject the other 128 single bytes, check the exact executable
set, exercise `nop` and `drop`, and verify rollback after a partially completed
two-stack `drop`.

## ABI coverage

This entry point does not emit `abi_facts`. In the [Wasm core execution model][wasm-invoke],
function calls consume typed operands, initialize the callee's parameter locals,
and return results to the operand stack. Function frames and control flow are
not modeled by this subset (`return` is encoding-only, and calls are absent).
There is no register file, stack-pointer register, or return-address register
for `abi_facts` v1. The operand and tag stack slots describe VM state, not a
native calling convention or host interface.

[wasm-invoke]: https://www.w3.org/TR/2019/REC-wasm-core-1-20191205/#exec-invoke

## References and cross-check

Opcodes come from the W3C WebAssembly Core Specification 1.0 sections on
[control](https://www.w3.org/TR/2019/REC-wasm-core-1-20191205/#binary-instr-control),
[parametric](https://www.w3.org/TR/2019/REC-wasm-core-1-20191205/#binary-instr-parametric),
and [numeric](https://www.w3.org/TR/2019/REC-wasm-core-1-20191205/#binary-instr-numeric)
instruction encodings. See its
[instruction index](https://www.w3.org/TR/2019/REC-wasm-core-1-20191205/#index-instr)
for the full instruction set and its
[text instruction grammar](https://www.w3.org/TR/2019/REC-wasm-core-1-20191205/#text-instr)
for WAT spellings.

All goldens were cross-checked using Binaryen `wasm-as` 133 with
`--mvp-features` in properly typed function bodies. For example, save the
following as `sample.wat` and use `wasm-as sample.wat --mvp-features -o sample.wasm`.
The instruction bytes at the end of Binaryen's function body are
`01 41 01 41 02 6a 1a 0b`: the two constants and the terminating `0b` are
provided by Binaryen but not supported by this Archon subset.

```wat
(module
  (func
    nop
    i32.const 1
    i32.const 2
    i32.add
    drop))
```

## Limits

This is an opcode codec, not a WAT parser, WebAssembly module assembler or
reader, arbitrary byte-stream decoder, validator, or general interpreter. It does not
handle LEB128 immediates or other multi-byte instructions, structured control
(including its `else`/`end` delimiters), locals, memory, typed `select`, or
post-MVP numeric instructions. Floating-point operators and conversions are
available only as opcodes, not as executable operations. There is no register
file; the two declared stacks are the complete modeled architectural state.
