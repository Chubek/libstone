# RV64 native MachineIR legalization

The legalizer checks that an instruction is directly supported by the native
backend. It does not insert conversions or rewrite the MIR. The verifier
checks value IDs, SSA, symbol declarations, and exact call signatures.

| MIR operation | Inputs | Result | Other fields |
| --- | --- | --- | --- |
| `CONST` | — | `i64` | `imm` is the complete 64-bit value |
| `COPY` | `i64`, `ptr`, or `func_ptr` | Same type as input | — |
| Integer operations (including comparisons, shifts and checked arithmetic) | `i64`, `i64` | `i64` | — |
| `TRUNC`, `ZEXT`, `SEXT` | `i64` | `i64` | `imm` is the logical width in bits (1–64) |
| `PTR_ADD` | `ptr`, `i64` byte offset | `ptr` | — |
| `LOAD` | `ptr` base | `i64` | `imm` is a signed byte displacement |
| `STORE` | `ptr` base, `i64` data | — | `imm` is a signed byte displacement |
| `FRAME_ADDR` | — | `ptr` | `imm` is an unsigned local index less than `local_count` |
| `GLOBAL_ADDR` | — | `ptr` | `symbol` names a global |
| `FUNC_ADDR` | — | `func_ptr` | `symbol` names a function |
| `CALL` | Typed argument list | As declared: `i64`, `ptr`, `func_ptr`, or no result (`void`) | `symbol` names the callee |
| `CALL_INDIRECT` | `func_ptr` callee in `a`, typed argument list | As declared: `i64`, `ptr`, `func_ptr`, or no result (`void`) | `symbol` names the declaration whose signature is used |

Unused fixed value operands must be `ARCHON_MIR_NONE`. Unused immediate fields
must be zero; unused symbol and argument fields must be null, with an argument
count of zero. Integers, data pointers and function pointers are distinct
types even though all occupy 64 bits; `EQ` is an integer comparison.

For `CALL_INDIRECT`, the verifier checks the signature of a statically known
function address, including through `COPY`. For other targets, the MIR
producer must ensure that the runtime address matches the declared signature.

For `LOAD` and `STORE`, `imm` contains the two's-complement bit pattern of the
signed displacement (for example, `UINT64_C(0) - 8` for −8, not `0xff8`).
Supported displacements are multiples of eight in the signed 12-bit range
(−2048 through +2040). The caller must ensure the **effective address** is
naturally aligned to eight bytes and valid for the access. `ptr` by itself
does not prove alignment; neither the verifier nor the lowering dynamically
checks it.

The conversion operations ignore all source bits above the logical width.
`TRUNC` and `ZEXT` both zero-extend the low `imm` bits to `i64`; they do not
create a narrower MIR type. `SEXT` instead fills the high bits with the sign
bit of the low `imm` bits. For example, with width 8, a low byte of `0x80`
becomes `0x80` under `TRUNC` or `ZEXT`, and `0xffffffffffffff80` under `SEXT`.
Width 64 is an identity for all three operations.

Arithmetic has RV64 bit-vector semantics. Shift counts use only their low six
bits. Unchecked `DIV_S`/`DIV_U` return all ones on a zero divisor; unchecked
`REM_S`/`REM_U` return the dividend. On signed overflow (`INT64_MIN / -1`),
`DIV_S` returns `INT64_MIN` and `REM_S` returns zero. `DIV_CHECKED_S` instead
traps on signed division overflow or a zero divisor. Checked signed addition
(`ADD_CHECKED_S`), subtraction (`SUB_CHECKED_S`), and multiplication
(`MUL_CHECKED_S`) trap on signed overflow.

Wrong types, unexpected fields, invalid widths or frame indices, and unaligned
displacements are invalid MIR. Unknown opcodes and aligned memory displacements
outside the target range are unsupported.
