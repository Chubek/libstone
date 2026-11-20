# AMD64 and AArch64 native scalar GAS

The opt-in native profile generates a C99 library and an `archon-backend`
driver. They accept [runtime MachineIR v1](MachineIR.md) (in-memory or text),
verify typed SSA, symbols, operand kinds, dominance, and call/edge signatures,
and lower directly to GAS in-process. The library does not invoke an assembler,
compiler, linker, or emulator. The legacy X-macro pipeline is separate.

| Input spec (exact canonical ArchDB) | Profile | Output |
| --- | --- | --- |
| [`stdspec/amd64/root.arch`](stdspec/amd64/root.arch) | `AMD64/SysVAMD64` integer LP64 | ELF GAS (AT&T), default or PIC |
| [`stdspec/aarch64/root.arch`](stdspec/aarch64/root.arch) | `AArch64/AAPCS64` integer LP64 | ELF GAS (little-endian A64), default or PIC |

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
  obtained function pointer must obey its declared MachineIR signature. All
  arguments and results use integer or pointer scalars; there are at most 64
  arguments and at most one result.

## Stack and symbols

Each SSA value has an 8-byte stack home. Edge copies first stage every source
in a dedicated frame area. The frame also reserves the largest outgoing
stack-argument area starting at pre-call SP. Homes, staging, locals, and
saved frame/return slots do not overlap. Fixed frames are limited to 64 KiB;
locals may require up to 16-byte alignment.

| ABI property | AMD64 | AArch64 |
| --- | --- | --- |
| Register arguments | `rdi`, `rsi`, `rdx`, `rcx`, `r8`, `r9` | `x0`–`x7` |
| First overflow argument, relative to incoming SP | `rsp + 8` | `sp` |
| Call boundary | `rsp` aligned to 16 before `call`; callee entry has residue 8 | `sp` aligned to 16 before and after `bl`/`blr` |
| Integer/pointer result | `rax` | `x0` |
| Return address | `call` pushes it; `ret` pops it | `x30`; non-leaf functions save it and preserve `x29` with an AAPCS frame record |

SysV AMD64 sets `%al` to zero before every call, including indirect calls,
because the MachineIR ABI passes no vector arguments. This lets a producer
declare a fixed all-scalar signature alias for a compatible external variadic
callee. The producer owns C promotions and signature compatibility; MIR does
not provide variadic function definitions, `va_start`, or general variadic ABI
classification.

Default address materialization assumes statically resolved, nearby code/data:
RIP-relative `lea` on AMD64 and `adrp`/`add` on AArch64. The assembly-only
`ARCHON_BACKEND_PIC` flag (`--pic`) materializes data and function addresses
through the ELF GOT: RIP-relative GOTPCREL loads on AMD64 and `adrp`/`ldr`
with GOT page/low relocations on AArch64. Direct calls use the ELF PLT where
required. Both targets leave object production, relocation, and range checks
to external tools.

There is no native raw encoder, physical record stream, scheduler, ABI
inference, alternate ABI, register allocator, dynamic frame allocation, FP,
SIMD, aggregates, atomics, unwind, or TLS in these two providers. Unsupported
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
and conversions to Python reference values, then execute the linked ABI, CFG,
and memory cases. AMD64 uses the native GNU assembler/linker and CPU. Its PIC
test links and loads a shared object and checks both data/function
interposition and the scalar variadic call protocol. AArch64 uses GNU cross
binutils if found or Clang's AArch64 integrated assembler and LLD, and runs in
native AArch64 Linux or QEMU user mode. The AArch64 PIC test checks dynamic GOT
and PLT relocations in a cross-linked shared object and executes the PIC code
in a statically linked binary. Set `ARCHON_QEMU_AARCH64` to a QEMU
executable outside `PATH`. These are regression checks, not general proofs of
translation correctness.
