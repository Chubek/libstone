# AMD64 and AArch64 native scalar GAS

The opt-in native profile generates a C99 library and an `archon-backend`
driver. They accept [runtime MachineIR v1](MachineIR.md) (in-memory or text),
verify typed SSA, symbols, operand kinds, dominance, and call/edge signatures,
and lower directly to GAS in-process. The library does not invoke an assembler,
compiler, linker, or emulator. The legacy X-macro pipeline is separate.

| Input spec (exact canonical ArchDB) | Profile | Output |
| --- | --- | --- |
| [`stdspec/amd64/root.arch`](stdspec/amd64/root.arch) | `AMD64/SysVAMD64` LP64 with SSE2 scalar FP | ELF GAS (AT&T), default or PIC |
| [`stdspec/aarch64/root.arch`](stdspec/aarch64/root.arch) | `AArch64/AAPCS64` LP64 with baseline scalar FP | ELF GAS (little-endian A64), default or PIC |

Generation checks the declared ABI v2 fields against the supported scalar
calling convention and pins a SHA-256 of the full, canonical ArchDB. A renamed
or altered database, missing ABI, or a different requested ABI is not accepted.
The selected stdspecs model only a few ISA instructions. The lowering rules
use reviewed baseline ISA sequences in addition to those instructions; there
are no claims that these rules were derived from ArchSem, or that the broader
instruction set can be encoded or emulated from the spec.

For example, from `cornerstone/Archon`:

```sh
python3 -m archon compile stdspec/amd64/root.arch -o amd64.archdb
python3 -m archon irgen gated --archdb amd64.archdb \
  --profile native --abi SysVAMD64 --no-oracle -o generated
cmake -S generated -B build
cmake --build build
build/archon-backend --input input.mir > output.s
```

Use `stdspec/aarch64/root.arch` and `--abi AAPCS64` for AArch64. The generated
`Backend.h` uses the same public C ABI and MachineIR schema as RV64. A static
library build for either new provider compiles the shared `Backend.c`,
`Legalizer.c`, `MIRText.c`, `NativeVerify.c`, and `ScalarGAS.c`, plus the selected
`AMD64GAS.c` or `AArch64GAS.c` and generated `NativeABIData.c`.
`NativeDriver.c` is only needed for the CLI.

## Covered MachineIR

- Arbitrary 64-bit constants and copies; wraparound add, subtract, multiply,
  bitwise operators, shifts masked to six bits, and normalized equality and
  signed/unsigned comparisons. Division and remainder are total, including zero
  divisors and signed minimum divided by −1. Checked signed add, subtract,
  multiply, and divide signal a breakpoint on failure.
- Integer truncation, zero extension, and sign extension for retained widths
  of 1–64; explicit, bit-preserving data/function pointer conversions. There
  are no implicit casts or pointer-kind interchange.
- Conditional and unconditional transfers, simultaneous block-parameter
  copies (including cycles), loops, scalar and void returns, and trap
  terminators.
- Naturally aligned 8/16/32-bit integer loads/stores and 64-bit integer, data
  pointer, and code pointer loads/stores with signed 12-bit displacements.
  Narrow loads extend explicitly. Clients supply valid, aligned effective
  addresses. Static locals and initialized, zero-filled, and external global
  words are supported, as are `ptr_add`, `global_addr`, and `func_addr`.
- Direct and indirect calls with validated declared signatures. A dynamically
  obtained function pointer must obey its declared MachineIR signature. There
  are at most 128 arguments and at most one result.
- IEEE binary32 and binary64 constants, arithmetic, negation, ordered
  comparisons, object loads/stores, format conversions, signed/unsigned
  integer conversions, and fixed-arity mixed FP/integer/pointer signatures.
  See [MachineIR](MachineIR.md#floating-point-operations) for exact types.

## Stack and symbols

Each SSA value has an 8-byte stack home, or 16 bytes for AMD64 `f80`.
Edge copies first stage every source
in a dedicated frame area. The frame also reserves the largest outgoing
stack-argument area starting at pre-call SP. Homes, staging, locals, and
saved frame/return slots do not overlap. Fixed frames are limited to 64 KiB;
locals may require up to 16-byte alignment.

| ABI property | AMD64 | AArch64 |
| --- | --- | --- |
| Integer/pointer argument registers | `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` | `x0`–`x7` |
| FP argument registers (separate bank) | `xmm0`–`xmm7` | low 32/64 bits of `v0`–`v7` (`s`/`d`) |
| First overflow argument, relative to incoming SP | `rsp + 8` | `sp` |
| Call boundary | `rsp` aligned to 16 before `call`; callee entry has residue 8 | `sp` aligned to 16 before and after `bl`/`blr` |
| Integer/pointer result | `rax` | `x0` |
| FP result | `xmm0` for `f32`/`f64`; `ST(0)` for `f80` | `s0` or `d0` |
| Return address | `call` pushes it; `ret` pops it | `x30`; non-leaf functions save it and preserve `x29` with an AAPCS frame record |

Each class fills its own bank before using stack slots. Overflow from both
classes shares one stack area in argument order with eight-byte slots, including
for `f32`. AMD64 `f80` arguments always take 16-byte-aligned, 16-byte stack
slots and consume no GP or SSE register. The FP rules are reviewed profile behavior; the pinned ABI v2 data
describes only the integer bank.

SysV AMD64 sets `%al` to the number of FP registers used before every call,
including indirect calls. A producer may declare a fixed scalar signature alias
for a compatible external variadic callee, but owns C promotions and signature
compatibility. MIR does not provide variadic function definitions, `va_start`,
or general variadic ABI classification.

Default address materialization assumes statically resolved, nearby code/data:
RIP-relative `lea` on AMD64 and `adrp`/`add` on AArch64. The assembly-only
`ARCHON_BACKEND_PIC` flag (`--pic`) materializes data and function addresses
through the ELF GOT: RIP-relative GOTPCREL loads on AMD64 and `adrp`/`ldr`
with GOT page/low relocations on AArch64. Direct calls use the ELF PLT where
required. Both targets leave object production, relocation, and range checks
to external tools.

There is no native raw encoder, physical record stream, scheduler, ABI
inference, alternate ABI, register allocator, dynamic frame allocation, extended
precision or complex FP, SIMD, aggregates, atomics, unwind, or TLS in these two providers. Unsupported
formats, invalid inputs, cancelled work, and limit violations return a
diagnostic and do not publish any artifact. The existing `assemble_object`
and `link_executable` Python bridges remain RV64-only.

## Independent verification

Run from `cornerstone/Archon`:

```sh
python3 -m unittest discover -s tests -p 'test_native_scalar_gas.py' -v
```

Tests build the generated host libraries with strict C99 flags and check
fingerprint/ABI failures, rejected output options, the in-memory C ABI,
cancellation, and atomic output limits. They compare emitted scalar arithmetic
and conversions to Python reference values, compare FP operations and mixed
register/stack calls against independently compiled C, and execute the linked
ABI, CFG, and memory cases. AMD64 uses the native GNU assembler/linker and CPU. Its PIC
test links and loads a shared object and checks both data/function
interposition and the scalar variadic call protocol. AArch64 uses GNU cross
binutils if found or Clang's AArch64 integrated assembler and LLD, and runs in
native AArch64 Linux or QEMU user mode. The AArch64 PIC test checks dynamic GOT
and PLT relocations in a cross-linked shared object and executes the PIC code
in a statically linked binary. Set `ARCHON_QEMU_AARCH64` to a QEMU
executable outside `PATH`. These are regression checks, not general proofs of
translation correctness.
