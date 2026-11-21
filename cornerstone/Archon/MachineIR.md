# Runtime MachineIR v1

The opt-in native backend accepts language-neutral, typed, in-memory SSA. It
does not require the generated IR runtime, compile-time includes, a fixed
working directory, or a downstream language runtime. `Backend.h` is the C API.
The eight existing IR libraries and the `Program.i` pipeline remain available
separately; their artifacts are not runtime MachineIR v1.

See [AMD64 and AArch64 scalar GAS](NativeScalarGAS.md) for those targets'
calling conventions, supported options, and limits. The physical-record,
scheduling, and RV64 assembly details below apply to the RV64 provider.

## Generic representation

In a function, value IDs index `types`; value types are `i64`, `ptr` (a data
address), `func_ptr` (a code address), `f32` (IEEE binary32), and `f64`
(IEEE binary64). FP types require a supported hardware floating-point ABI:
AMD64/SysVAMD64 or little-endian AArch64/AAPCS64. The RV64I and RV64IM LP64
providers reject FP types, including FP imports; they do not lower to soft-float
helpers or emit instructions from F or D. Function arguments define the first
IDs. Other definitions are block parameters or
instruction destinations. A value may be used only after its definition, or in
a block dominated by that definition. Entry is block zero. Each non-entry block
parameter receives an argument on each predecessor edge; the entire assignment
is simultaneous, including on backedges. Entry has no block parameters or
incoming edges, and every block must be reachable from entry. A conditional
branch takes its `yes` edge for any nonzero `i64`. Each block has an explicit
terminator. `void` is permitted in function and import result positions only.

Unused instruction value fields and unused terminator values must be
`ARCHON_MIR_NONE`, not zero. Other unused instruction fields are zero or
`NULL`. Symbol references must match a declared function/import or data symbol
of the correct kind. External signatures are contracts supplied by the caller;
the backend cannot inspect separately compiled function implementations.

- `add`, `sub`, `mul` wrap at 64 bits; bitwise ops act on the whole word.
  Shifts use the low six bits of the count. Comparisons produce 0 or 1.
- Division and remainder are explicitly total: division by zero returns an
  all-ones quotient and remainder returns the dividend. Signed `MIN / -1`
  returns `MIN`, with zero remainder. The checked signed add, subtract,
  multiply, and divide operations trap on overflow, and checked divide also
  traps on zero.
- `trunc DST SRC WIDTH` and `zext DST SRC WIDTH` both retain the low `WIDTH`
  bits of `SRC` and clear the rest of the `i64` result; `sext` fills the rest
  from the highest retained bit. Width must be in 1–64. These operations do not
  change the carrier type or implicitly convert pointers.
- `ptr_add` adds a byte offset modulo the machine address width. `load_s8`,
  `load_s16`, and `load_s32` read one, two, or four little-endian bytes and
  sign-extend to `i64`; the corresponding `load_u*` operations zero-extend.
  `store8`, `store16`, and `store32` write the low bits of an `i64`. Existing
  `load`/`store` transfer an `i64` word. `load_ptr`/`store_ptr` transfer a data
  pointer, while `load_func_ptr`/`store_func_ptr` transfer a function pointer;
  the types are not interchangeable even though all full-width transfers use
  an eight-byte representation in the supported LP64 profiles.
- `ptr_to_int`/`int_to_ptr` and
  `func_ptr_to_int`/`int_to_func_ptr` explicitly preserve a 64-bit pattern
  while changing between one pointer kind and `i64`. Each pair accepts only
  its exact pointer kind; there is no data-pointer/function-pointer cast.
  For the supported ELF profiles, converting integer zero to `func_ptr` produces
  the null function pointer. A nullable function pointer can therefore be
  converted to `i64` and compared with an `i64` zero using `eq`.
- Memory displacements are signed 12-bit byte offsets aligned to the access
  width. Clients must supply valid, naturally aligned effective addresses and
  sufficient stack or global storage. No volatile, atomic, aliased
  source-language object, pointer provenance, or bounds-checking policy is
  inferred.
- `frame_addr` takes an index in the current function's local table.
  `global_addr` names a defined or external data object. Global data consists
  of eight-byte words, initialized or zero-filled.
- `call` is a direct fixed-signature LP64 call. `func_addr` materializes a
  declared code address. `call_indirect` names a declaration for the expected
  fixed signature and calls the `func_ptr` value, not the declaration symbol.
  If a `func_addr` is statically visible through copies, the backend rejects a
  mismatching signature. The producer must establish that all dynamic targets
  satisfy the signature; the backend cannot verify the actual run-time address.
  Functions, imports, and calls accept up to `ARCHON_MIR_MAX_PARAMS` (128)
  scalar ABI parameters or arguments.
   `ret -` returns void.
   `trap` raises the target breakpoint exception.

### Floating-point operations

| Text instruction | Contract |
| --- | --- |
| `fconst DST BITS` | Load the IEEE representation verbatim; `f32` requires `BITS <= 0xffffffff`. |
| `fload DST PTR DISP`; `fstore PTR SRC DISP` | Transfer the representation verbatim, at the FP value's width (4 or 8 bytes). Standard memory alignment and displacement rules apply. |
| `fneg DST SRC` | Flip the sign bit, preserving the rest of the representation, including NaN payload and zero. |
| `fadd/fsub/fmul/fdiv DST A B` | Use the matching format for both operands and result; round each operation in that format. No implicit widening, reassociation, or contraction. |
| `feq/flt/fle DST A B` | Ordered comparison of operands of the same format, yielding `i64` 0 or 1. `+0` and `-0` compare equal; unordered (either operand NaN) yields 0. Negate `feq` to implement C `!=` or a truth test against zero. |
| `fcast DST SRC` | Convert between different FP formats, rounding on narrowing. |
| `sitofp/uitofp DST SRC` | Convert a signed/unsigned full-width `i64` to the destination FP format. Narrow integer types need explicit sign/zero extension first. |
| `fptosi/fptoui DST SRC` | Truncate toward zero and convert to signed/unsigned full-width `i64`. NaN, infinity, and a truncated value out of range have no specified result. Check or narrow explicitly as required by the source language. |

`copy`, branches with FP edge arguments, returns and fixed-arity calls preserve
the FP value's type. `cbr` still requires `i64`. Hardware arithmetic and rounding
conversions use the target FP environment (assumed default for the supported C
subset); target NaN arithmetic payloads and exception flags are not portable
MIR results. These operations provide the scalar building blocks for C11
§6.3.1.2 and §6.3.1.4–8; source-language promotions, constant translation
(§6.4.4.2), and floating-environment or Annex F guarantees are not inferred.

## Runtime text interchange

The same C API can ingest a bounded, length-delimited `archon-mir 1` token
stream with `archon_backend_compile_text`. It is not an X-macro file and is
never preprocessed. Only ASCII spaces, tabs and line endings separate tokens.
There are no comments, quoting, directives or include mechanisms. Counts and
indices are unsigned decimal; constant words and displacements also accept
`0x` hex bit patterns. `-` means no SSA value (void calls and returns).

```text
archon-mir 1
module 1 0 0
function add_two i64 2 3 1 0
types i64 i64 i64
block 0 1
params
add 2 0 1
ret 2
end
```

After `module` come the function, import and global counts, then those records
in that order. After a function's name and result type come the function-
argument, value, block and local counts. `types` has one type per value, then
one `local SIZE ALIGNMENT` record per local. A `block` header gives its
parameter and instruction counts; `params` is followed by the parameter IDs.

Instruction operands are `dst`, `a`, `b` (omitting unused fields), followed by
the applicable immediate or symbol. The exceptions in spelling are
`call DST_OR_- SYMBOL COUNT ARGS...` and
`call_indirect DST_OR_- FUNC_PTR_VALUE SIGNATURE_SYMBOL COUNT ARGS...`.
The terminators are:

```
ret VALUE_OR_-
br TARGET ARG_COUNT ARGS...
cbr VALUE TRUE_TARGET TRUE_COUNT TRUE_ARGS... FALSE_TARGET FALSE_COUNT FALSE_ARGS...
trap
```

Imports use `import SYMBOL RESULT COUNT PARAM_TYPES...`. Globals use
`global SYMBOL COUNT zero` or `global SYMBOL COUNT init VALUES...`. Count zero
with `zero` declares an external data symbol. Defined symbols are exported.
The stream ends with `end`; extra tokens are rejected. The supported opcode
names are the lower-case `ARCHON_MIR_*` names in `Backend.h` (without the
prefix), excluding the separately spelled terminators. New v1 operations are
appended to the public enum so existing opcode values and text remain
unchanged.

## Lowering contract

Input verification checks the typed SSA before code generation. The first
provider selects physical operations against the reviewed native target;
allocation determines value homes ahead of selection. It then emits checked
physical operands to the assembly sink. Selected and allocated instructions
are internal lowering operations, not public mutable MIR interchange phases.
For inspection, `ARCHON_BACKEND_PHYSICAL_RECORDS` publishes read-only physical
records instead of assembly. Native scheduling preserves exact incoming order
by default; `ARCHON_BACKEND_SCHEDULE_PURE` enables checked reordering inside
pure regions only. Neither infers hardware latency. An unsupported operation
or failed compatibility check prevents publishing an artifact.

Assembly output defaults to the existing non-PIC contract (`lla` for data or
function addresses). `ARCHON_BACKEND_PIC` instead emits `.option pic` and GAS
`la`, allowing the assembler to use GOT-based address materialization for
preemptible external symbols. It does not change MIR pointer types or call
signatures. PIC is rejected with physical records or raw machine code because
those modes neither represent nor apply external GOT relocations.
