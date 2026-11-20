# Archon language subset

[Toy8](examples/toy8.arch) is a complete, small architecture description. To
try it from `cornerstone/Archon`:

```sh
python3 -m archon compile examples/toy8.arch -o toy8.archdb
python3 -m archon asm toy8.archdb 'add r1, r2, r3'  # 5302
python3 -m archon run toy8.archdb 'add r1, r2, r3' --reg r2=255 --reg r3=2
```

The execution writes `1` to `r1`: the sum wraps at eight bits.

## Architecture model

- A source starts with `architecture Name { ... }`. `import isa.integer;`
  loads `isa/integer.arch` relative to the importing file. Imported files start
  with `module Name { ... }`. Declarations share one namespace; duplicate names
  and cyclic imports are errors.
- Set `endian` to `little` or `big` and define `address_width`. A parameter
  like `parameter XLEN: nat in {32, 64} = 64;` provides a configurable width.
  A `type` alias can refer to `bits<N>`, `uint<N>`, `sint<N>`, or another alias.
- Features are off unless declared `feature Base = enabled;` or selected on the
  command line. `feature D requires F;` specifies a prerequisite; the compiler
  checks rather than automatically enabling prerequisites.
- An architecture may declare one contiguous, zero-based `register_file` with
  a count and width. It may instead omit registers entirely. A `zero` index
  reads as zero and discards writes. Register aliases are accepted as input;
  disassembly prints the numbered register spelling. A `register_class` can
  include all members of the file.
- Named non-register state is explicit and bounded:

  ```arch
  state PC: bits<16>;
  state Carry: bits<1>;
  stack OperandStack: bits<32> { capacity = 256; }
  memory MainMemory { size = 65536; }
  ```

  A `state` cell and a stack element may use any integer bit-vector type or
  type alias. Stack capacity is an element count. Memory size is a byte count;
  memory is byte addressed and uses the architecture's `endian` setting for
  multi-byte accesses. Capacities and sizes may refer to `nat` parameters.
- A `format` is a byte-aligned bit vector. Its contiguous fields must cover
  every bit exactly once. Each instruction binds every format field to an
  integer constant or a single operand; `encode_register(reg)` and a bare
  register operand both bind its index. An immediate such as
  `in delta: imm<sint<12>>;` must match its field's width and signedness.
- An instruction needs `encoding` and an `assembly` template naming all its
  operands. Only enabled instructions are emitted, and their encodings cannot
  overlap. All enabled instructions in the current model have the same width.
  Omitting `semantics` is legal, but such instructions cannot be executed.

## Instruction templates

`template` separates a reusable instruction body from its name and encoding.
Toy8 defines `template Sum<Name: string, Opcode: bits<7>>` with an
`instruction self` body. It uses `Opcode` as an encoding constant and
`{Name}` in the assembly template. `def ADD = Sum<"add", 1>;` gives the
instruction its final name and binds those parameters. Operand placeholders
such as `{dst}` still refer to operands at assembly time. Instantiated
instructions undergo the same type, encoding, and overlap checks as any
other instruction. Arguments are positional literals: `string` for assembly
text, `bits<N>` or `nat` for encoding constants, and `nat` for immediate
widths. Templates are visible through imports. Operation arguments, nested
templates, and `foreach` are not yet supported.

## ArchSem

Statements run in source order. A `let` binds a local, optionally with an
exact type annotation. `read` accepts `in` or `inout` register operands, and
`write` accepts `out` or `inout` register operands. Reads and integer literals
are default-word-width `bits`; the default is the register width when a
register file exists and the address width otherwise. Immediates carry their
declared width and sign.

`read_state(PC)` reads a named scalar cell. `write_state(PC, value);` writes it,
and `PC := value;` is equivalent shorthand. The value must have a compatible
width and type: `bits` can supply a neutral bit pattern, but `sint` and `uint`
are not implicitly interchangeable. `pop(OperandStack)` removes and returns
the top stack element; `peek(OperandStack)` reads it, and
`peek(OperandStack, 1)` reads one element below it.
`push(OperandStack, value);` appends a compatibly typed value. Underflow and
overflow fail the instruction.

Memory operations name their transferred type:

```arch
let word = load<bits<32>>(MainMemory, address);
store<bits<32>>(MainMemory, address, word);
```

The transfer width must be a multiple of eight. Addresses must be unsigned
bit vectors of exactly `address_width`. Accesses that extend beyond the
declared byte size fail; little- and big-endian targets assemble the bytes in
their declared order.

ArchSem supports wrapping `+%`, `-%`, and `*%`, bitwise `~`, `&`, `^`, and `|`,
and shifts `<<` and `>>`. Right shift is arithmetic on `sint`, logical
otherwise. Counts greater than or equal to the input width produce all fill
bits. `sext<N>`, `zext<N>` and `trunc<N>` explicitly change widths.
`extract(value, high, low)` selects an inclusive bit range with literal
indices; `concat(high, low)` appends the lower bit vector after the higher
one. Register writes require a register-width result; the compiler derives
the register, scalar, stack, and memory resources read and written from these
statements.

An explicit `sequence` runs its statements in order. In a `parallel` block
every right-hand side reads the pre-block state; register writes, scalar writes,
and memory stores commit together. Conflicting writes to one register, scalar
cell, or memory byte fail. Toy8 uses this for `SWAP`:

```arch
parallel {
    lhs := read(rhs);
    rhs := read(lhs);
}
```

Writes to the hardwired zero register are discarded. If two operands refer to
the same writable register, a `parallel` block can write it twice only when
the two values agree. Blocks have lexical scope: their local bindings cannot
shadow operands, state resources, or other bindings, and cannot escape.
Stack mutation is ordered: `pop` and `push` are not permitted in `parallel`;
`peek` is permitted. A `parallel` block contains only `let`, register/scalar
writes, and memory stores, while `sequence` may contain other blocks.

Execution is transactional: stack or memory faults and conflicting parallel
writes expose none of that instruction's updates. Instruction-level control
flow, implicit PC advancement, and exceptions are not modeled yet.
The root [design document](../../AGENTS.md) describes the broader language
and its roadmap.

## Assembly files

[The Toy8 assembly example](examples/toy8.s) uses local labels as immediate
values. A bare `label` is its absolute byte address; `@label` is its byte
offset from the start of the instruction using it. Labels can appear before
or after their uses. In the example, the first `movi` loads `6` and the
second loads `-2` at base address zero. These are numeric immediate
fixups, not branch instructions or linker relocations. Archon checks the
resolved value against the declared immediate's width and signedness.
Either label form accepts a single numeric addend, such as `end + 2` or
`@start - 2`. Blank lines and `#` comments are ignored.

The assembled bytes are a raw sequence of fixed-width instructions. The
file contains no labels, sections, symbols, or object metadata; disassembly
therefore prints numeric operands rather than recreating labels.
