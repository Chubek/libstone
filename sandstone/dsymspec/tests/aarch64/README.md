# AArch64 profile and LLVM checks

The standalone `aarch64.dsymspec` profile targets little-endian A64,
ELF64/AAPCS64, and defaults to `base`, `fp`, and `neon` (Armv8.0-A plus
FP and Advanced SIMD). It is not an aggregate Armv9 profile. System
forms explicitly marked `priv` are opt-in; the selected exception level,
implemented ELs, and system-register access permissions are separate CPU
state. Neither A32/T32 nor AArch64 ILP32/big-endian is described here.

Coverage was organized against the [Arm A64 instruction set
index](https://developer.arm.com/documentation/ddi0602/latest/), the
[LLVM 22 AArch64 instruction inventory](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.8/llvm/lib/Target/AArch64/AArch64InstrInfo.td),
and the [AAELF64 relocation specification](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst).

### Binding convention

The standalone header expects a caller to match forms and normalize
operands; it does not parse GAS. For typed recipes, `r0`, `r1`, … are in
operand order. Register values are encoding numbers. Constraints `x`/`w`
interpret 31 as ZR; `xsp`/`wsp` and memory bases interpret it as SP. The
number alone cannot distinguish those spellings, or distinguish physical
registers from other register classes. Legacy compatibility encoders
keep their prior names and bindings.

- Memory forms use `disp` in bytes. `memN` is a scaled offset (signed
  for pairs); `memoffN`, `mempreN`, and `mempostN` select signed offset,
  pre-index, and post-index encodings. `memregN` binds `index`, `extend`
  (2 = UXTW, 3 = LSL/UXTX, 6 = SXTW, 7 = SXTX), and `scaled` (0/1,
  including explicit `lsl #0` on byte accesses). The matcher checks the
  index register width and the spelled shift against the access size.
- Numeric branch, literal-load, ADR, and ADRP operands represent
  PC-relative byte deltas (page delta in bytes for ADRP). Symbolic forms
  instead emit a relocation at the instruction offset and take `sym`.
  `sysop` packs `op1:CRn:CRm:op2`; `sysreg` also includes `op0`.
- Logical immediates use the packed `N:immr:imms` field. Raw masks for
  the logical instructions must be normalized by the caller (there is
  also a legacy two-value numeric EOR form). `mov` has a separate
  numeric encoder for constants representable by one MOVZ/MOVN;
  normalized `mov … logical` aliases ORR. `fimm8` is the FP immediate
  field, not a host floating-point value. `simdimm8` uses that field
  with `cmode` and a form-specific arrangement.
- SIMD/SVE forms carrying `arr` encode `8B,16B,4H,8H,2S,4S,1D,2D` as
  `0..7`; when widening/narrowing, pass the narrower operand's
  arrangement. Dot products use `arr` only for Q (0/1). `esize` is
  `0..3` for B/H/S/D when a lane or scalable element size is needed.
  Check vector suffix relationships, lane/list geometry, and predicate
  qualifiers when matching. For lists, bind the first register and
  `nregs` where required. Lane bindings are `lane` and `lane_src`.
  Structure-list `disp` is zero; post-index forms bind the explicit
  increment or register as a separate operand.

### What is and is not covered

- **Default A64:** W/X arithmetic and logical forms, carry and multiply
  families, bitfield/shift and conditional families, branches and
  PC-relative address generation; integer and SIMD/FP scaled,
  unscaled, unprivileged (integer), writeback, register-offset,
  literal, pair, and exclusive/ordered accesses; exception, hint,
  barrier, and raw system encodings with selected named registers.
  Not every assembler alias, named cache/TLBI/AT operation, named
  system register, or exception-level rule is cataloged.
- **FP and Advanced SIMD:** scalar S/D arithmetic, compare, rounding,
  transfers and conversions (including fixed-point GPR conversions);
  vector arithmetic, logic, widening/narrowing, reductions, shifts,
  lane moves, table lookup, permutations, and structure loads/stores.
  Not exhaustive: notably indexed-element arithmetic/multiply/FMA,
  vector fixed-point conversions, some scalar SIMD conversion and
  pairwise forms, and aliases such as widening shifts by element size.
- **Optional features:** family coverage for LSE byte/halfword/W/X
  atomics and CAS pairs, CRC32/CRC32C, AES/PMULL, SHA, SM3/SM4,
  baseline RCpc and LRCPC2, classic pointer authentication, core MTE
  addressing and tag operations, CSSC, FlagM, BTI, SB, SSBS (MSR),
  DIT (MSR), RNG, PREDRES, FP16, BF16, dot and I8MM. LS64 covers
  LD64B/ST64B but not LS64_V/LS64_ACCDATA. Later feature revisions
  (e.g. LSE128, RCpc3, PAuth-LR, MTE bulk tag access) are not covered.
  Indexed dot/BF16 forms and several FP16 indexed/reduction families
  remain absent.
- **Scalable extensions:** initial SVE predicate/count/select/basic
  arithmetic, SVE2 widening add/subtract and HISTCNT, SME streaming
  control/ZA zero/selected outer products, one SME2 ZA vector-group
  ADD form. Missing scalable memory (including gather/scatter and
  first-fault), most predicate operations and predicated FP, most of
  SVE2/SME2, ZA slice/array transfers and ZA/ZT0 register topology.
  The Z/P register widths in the catalog are minimum sizes, not a
  fixed vector-length model.
- **ELF:** instruction recipes cover branch, literal, ADR/ADRP, GOT,
  ADD/LDST low-12, and selected MOVW relocations. TLS and dynamic
  entries without instruction recipes are metadata only. The caller
  still owns ELF numbering/layout, addend handling, and relocation
  application; this is not a full AAELF64 relocation table.

### Reproduce

After generating the header (see `sandstone/dsymspec/README.md`), run:

```sh
python3 sandstone/dsymspec/tests/aarch64/check.py build/dsymspec/dsymspec/generated
```

The driver compiles the header under strict C11, verifies selected
feature requirements and executable encoder declarations, then checks
bytes and ELF relocation tuples against `llvm-mc` and `llvm-readobj`.
The rejection cases exercise the generated encoders with invalid
normalized operands. Fixtures use LLVM 22 feature names. This is a
representative check, not an exhaustive ISA conformance test.
