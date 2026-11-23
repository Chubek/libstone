# Binary128 compiler/runtime interface

MIR uses `f128` and transports its two `i64` words with `f128_pack`,
`f128_low`, and `f128_high` (least significant word first). Helper calls use
the target's public `long double` ABI for arguments and results.

| Symbols | Signature |
| --- | --- |
| `__addtf3`, `__subtf3`, `__multf3`, `__divtf3` | `(long double, long double) -> long double` |
| `__letf2`, `__getf2`, `__eqtf2`, `__unordtf2` | `(long double, long double) -> long` on RV64; `int` on AArch64 |
| `__extendsftf2`, `__extenddftf2` | `(float or double) -> long double` |
| `__trunctfsf2`, `__trunctfdf2` | `(long double) -> float or double` |
| `__floatditf`, `__floatunditf` | `(int64_t or uint64_t) -> long double` |
| `__fixtfdi`, `__fixunstfdi` | `(long double) -> int64_t or uint64_t` |

Ordered comparison returns a signed three-way result. On unordered inputs
`__letf2` and `__eqtf2` return +1, `__getf2` returns -1, and `__unordtf2`
returns 1. Equality includes opposite signed zeros. The bundled integer
implementations use nearest-even rounding and do not expose FP exceptions.
NaN results are quiet; payload preservation is not guaranteed. Underflow is
gradual. Integer conversion truncates toward zero for representable results.

`src/real_bits.c` is the Libstone integer implementation shared with the host
constant folder. It uses the bootstrap C compiler's unsigned 128-bit integer
type, never wide division, native floating arithmetic or external arithmetic
libraries. `src/real_abi.c` converts target C arguments and results between
their public ABI types and raw words. No ABI helper is linked into the host
compiler. The binary32/64 runtime entries use the same core.
