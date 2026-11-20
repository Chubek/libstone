# RV64 native MachineIR legalization

The legalizer checks that an instruction is supported by the generated native
profile. It does not insert conversions or rewrite the MIR. The verifier
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
| `LOAD_S8`, `LOAD_S16`, `LOAD_S32` | `ptr` base | Sign-extended `i64` | `imm` is a signed byte displacement |
| `LOAD_U8`, `LOAD_U16`, `LOAD_U32` | `ptr` base | Zero-extended `i64` | `imm` is a signed byte displacement |
| `STORE8`, `STORE16`, `STORE32` | `ptr` base, `i64` data | — | Stores the low 8, 16, or 32 bits; `imm` is a signed byte displacement |
| `LOAD_PTR`, `LOAD_FUNC_PTR` | `ptr` base | `ptr` or `func_ptr`, respectively | `imm` is a signed byte displacement |
| `STORE_PTR`, `STORE_FUNC_PTR` | `ptr` base, matching pointer data | — | `imm` is a signed byte displacement |
| `PTR_TO_INT` | `ptr` | `i64` | Preserves the RV64 bit pattern |
| `INT_TO_PTR` | `i64` | `ptr` | Preserves the RV64 bit pattern |
| `FUNC_PTR_TO_INT` | `func_ptr` | `i64` | Preserves the RV64 bit pattern |
| `INT_TO_FUNC_PTR` | `i64` | `func_ptr` | Preserves the RV64 bit pattern |
| `FRAME_ADDR` | — | `ptr` | `imm` is an unsigned local index less than `local_count` |
| `GLOBAL_ADDR` | — | `ptr` | `symbol` names a global |
| `FUNC_ADDR` | — | `func_ptr` | `symbol` names a function |
| `CALL` | Typed argument list | As declared: `i64`, `ptr`, `func_ptr`, or no result (`void`) | `symbol` names the callee |
| `CALL_INDIRECT` | `func_ptr` callee in `a`, typed argument list | As declared: `i64`, `ptr`, `func_ptr`, or no result (`void`) | `symbol` names the declaration whose signature is used |

Unused fixed value operands must be `ARCHON_MIR_NONE`. Unused immediate fields
must be zero; unused symbol and argument fields must be null, with an argument
count of zero. Integers, data pointers and function pointers are distinct
types even though all occupy 64 bits; `EQ` remains an integer comparison. Each
explicit pointer/integer cast accepts exactly one pointer kind. There is no
cast between `ptr` and `func_ptr`, so code and data addresses are never
silently interchangeable. For the RV64 ELF native profile, integer zero
converted by `INT_TO_FUNC_PTR` is the null function pointer; null comparison is
`FUNC_PTR_TO_INT` followed by integer `EQ` with zero.

For `CALL_INDIRECT`, the verifier checks the signature of a statically known
function address, including through `COPY`. For other targets, the MIR
producer must ensure that the runtime address matches the declared signature.
This promise also applies to any non-null value produced by
`INT_TO_FUNC_PTR`; the cast establishes the representation, not a call
signature or executable-address proof.

For every memory operation, `imm` contains the complete two's-complement
64-bit pattern of the signed displacement (for example, `UINT64_C(0) - 8` for
−8, not `0xff8`). It must fit signed 12 bits and be a multiple of the access
width. Thus byte offsets range through +2047, halfword offsets through +2046,
word offsets through +2044, and eight-byte offsets through +2040; −2048 is
valid for every width. The caller must ensure the **effective address** is
naturally aligned and valid for the access. A displacement aligned relative
to an unknown base does not prove this; neither verifier nor lowering inserts
a dynamic check.

Frame locals may have byte-granular sizes and power-of-two alignment through
the LP64 stack alignment of 16, so producers can declare suitably aligned
one-, two-, four-, or eight-byte local objects before using `frame_addr`.
Globals remain arrays of eight-byte little-endian words with eight-byte symbol
alignment. Narrow operations can address bytes inside that declared extent,
but MachineIR v1 has no byte initializer or typed global layout; producers
must pack initial bytes into words and remain within the allocated word count.

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

RV64IM implements all of these operations. RV64I lowers modular `MUL` to a
bounded shift-and-add sequence, but rejects `DIV_S`, `DIV_U`, `REM_S`, `REM_U`,
`DIV_CHECKED_S`, and `MUL_CHECKED_S` at verification with
`ARCHON_BACKEND_UNSUPPORTED`. No helper runtime is linked implicitly.

Wrong types, unexpected fields, invalid widths or frame indices, and unaligned
displacements are invalid MIR. Unknown opcodes and aligned memory displacements
outside the target range are unsupported.
