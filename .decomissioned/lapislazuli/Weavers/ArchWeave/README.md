# ArchWeave

A Lua 5.4 DSL for **32-bit fixed-width instruction codecs**, emitting C99.
Load with `require("archweave")` or `require("ArchWeave")` after adding this
directory to `package.path`. See [ArchCollection](../../ArchCollection/README.md).

```lua
local w = require("archweave")
return w.arch {
  name="Tiny", arch="example", machine=0, word_bits=32, address_bits=32,
  endian="big", source="your ISA reference",
  instructions={
    { name="load", match=0x10000000, mask=0xffff0000,
      operands={ { name="imm", shift=0, bits=16, signed=true } } },
  },
}
```

`w.arch(table)` snapshots and validates the description. `w.load(path)` loads a
Lua file returning a description. `description:emit_header()` and `:emit()` emit
a public header and C implementation including `NAME.h`. Reorder instruction
declarations without changing output; operands retain their declared order.

* Namespaces start with an uppercase ASCII letter; mnemonics and operand names
  start lowercase. Unknown attributes, sparse lists and duplicate names fail.
* `arch`/`machine` identify the BFD family/machine; they are **not ELF EM IDs**.
  `word_bits`/`address_bits` describe the architecture, not variable instruction
  size. Every supported instruction is exactly four bytes.
* `match` and `mask` are unsigned 32-bit integers. Match bits must lie inside
  the mask. Operand `shift`/`bits` fields and fixed bits must cover all 32 bits
  exactly once. Ambiguous encodings, including aliases, are rejected.
* `signed` defaults false; `scale` is 1 (default), 2, 4 or 8. Values are decoded
  as signed/unsigned bit fields and then scaled. Encode rejects out-of-range
  values and misalignment rather than truncating. At most six operands.
* `source` carries a reference string into the generated metadata. Constructors
  accept integer-valued **Lua integers**, not floating-point values.

Generated functions (replace `NAME` with the namespace):

| API | Purpose |
| --- | --- |
| `NAME_info()` | BFD identity, word/address size, default endian, provenance |
| `NAME_instructions(&count)`, `NAME_lookup("name")` | Immutable instruction/operand metadata |
| `NAME_encode_word(name, values, count, &word)` | Numeric word, independent of host endian |
| `NAME_decode_word(word, &decoded)` | Encoding selector and ordered operand values |
| `NAME_encode(name, values, count, bytes, size, little)` | Four target-order bytes |
| `NAME_decode(bytes, size, little, &decoded)` | Unaligned-safe byte decoding |

Byte functions require explicit `little` = 0/1; architecture metadata provides
the default. Status is 0 on success, -1 unknown mnemonic/encoding, -2 short
buffer, -3 invalid argument/operand. Outputs are unchanged on error. Metadata
and decoded instruction pointers have static lifetime; no allocations or
mutable global state. Buffers and output objects must not overlap.

There is no textual assembler, relocation engine, PC resolution, emulator,
variable-length decoder, alias selection, feature inference or complete ISA
verification. Scaled branch displacements remain offsets; consult each profile
for its PC base. A decoded instruction is not permission to execute it.
