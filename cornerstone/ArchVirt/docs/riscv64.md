# RISC-V DomIOS execution contract

Select `--machine riscv64 --backend software` (`rv64` and `riscv` are aliases).
`auto` selects software; explicit `kvm` fails. This is a flat, little-endian,
single-hart RV64IM + Zifencei execution environment, implemented in
`src/virtual/riscv64_cpu.{hpp,cpp}` and integrated with the shared loader,
runtime, C API, CLI, shell, embedded/file images, and managed IPC.

## Boot and devices

ELF64 `ET_EXEC`, machine 243, flags zero (LP64 soft-float, no RVC/RVE/TSO),
executable four-byte-aligned entry, and bounded identity-mapped load segments
are required. Kernel load window is [0x200000,0x600000); boot information is
at 0x10000, initial `sp`/x2 at 0x700000, and `a0`/x10 contains 0x10000. Other
GPRs start at zero. This is **not** Linux's hart-ID/DTB entry convention.
Image data begins at 0x800000; optional validated firmware at 0x2000000.
The 96-byte `ArchVirtBootInfo` reports machine 243 and serial base 0x10000000.

Only byte reads/writes access the polled serial MMIO register. A read returns
zero when no input is available; a byte write emits serial output. Wider
accesses to the register fail without a device operation. The 80x25 console
at 0xb8000 is ordinary guest RAM. There is no 16550 register bank, CLINT, PLIC,
SBI, virtual memory, privileged CSR state, interrupt delivery, or trap handler.

## Instructions and failures

- RV64I integer ALU/immediates, W forms, branches, JAL/JALR, LUI/AUIPC,
  signed/unsigned loads and byte/half/word/doubleword stores.
- M multiply (including signed/mixed/unsigned high halves), division and
  remainder, with specified divide-by-zero and signed-overflow results.
- FENCE and FENCE.I: the one synchronous hart has no queued memory operations
  or instruction cache. New instruction bytes are fetched on every step;
  decode caching is keyed by the instruction word, not the PC. Reserved operand
  fields of these fences are ignored as required for base implementations.
- Exact `EBREAK` (0x00100073) is the DomIOS **terminal halt convention**, not a
  breakpoint exception. It advances PC and counts as one completed instruction.
- ECALL, other SYSTEM instructions, A/C/F/D/V and other extensions fail. The
  ArchML description may decode wider metadata; the execution adapter is the
  final admission check and rejects reserved funct combinations.

IALIGN is 32. Taken misaligned targets fail before writing link registers;
untaken branches do not check their unused target. JALR clears target bit zero
before checking alignment. Data accesses must be naturally aligned, even if
they would fit RAM. Reads into x0 still perform device accesses and faults.
Stores validate the whole range before writing; a failing instruction preserves
GPRs and PC, reports PC/encoding/reason, and leaves the VM `failed`. Prior
instructions' effects remain. Device callbacks must be bounded and nonthrowing;
external callback side effects cannot be rolled back. No guest recovery exists.

IPC `query-registers` returns x0..x31 plus pc/sp. Existing pause, step, memory,
console, reset-generation, instruction-limit, and result semantics apply. An
instruction budget is host execution control, not guest timer preemption.

## Build and verification

From the book root:

```sh
cmake -S archvirt -B archvirt/build-riscv -DCMAKE_BUILD_TYPE=Release
cmake --build archvirt/build-riscv -j6
TMPDIR=/tmp/opencode ctest --test-dir archvirt/build-riscv --output-on-failure
TMPDIR=/tmp/opencode make -C nova-dos test run run-n3
```

Guest compiler flags: `--target=riscv64-none-elf -march=rv64im_zifencei
-mabi=lp64 -mcmodel=medany -mno-relax -msmall-data-limit=0`. No target libc is
needed. Domix is built/embedded for all three profiles. Nova-DOS uses the local
`build-riscv/archvirt` by default; `ARCHVIRT` (Make) or `ARCHVIRT_BIN` (Python)
can select another build supporting this contract.

Eight CPU unit cases check integer widths/signs, link aliasing, reserved forms,
fault atomicity, memory/MMIO, self-modification, and M arithmetic against wider
host arithmetic. CTest also checks bundled boots, C API, image rejection, IPC
stepping/reset/binary console/ring overflow/watchdogs, and retained x86/AArch64
and native x86 KVM execution. Nova-DOS adds actual RV64 compiler output,
FAT policy comparisons, NEX rejection/publication, callback and LP64 bridge
experiments. These establish this execution environment, not privileged ISA
or physical RISC-V conformance.

Semantic references: RISC-V Unprivileged ISA 20260120, RV32I/RV64I 2.1,
M 2.0 and Zifencei 2.0 chapters
(<https://docs.riscv.org/reference/isa/v20260120/unpriv/rv64.html>),
and the integer LP64 calling convention/ELF machine flags in the retrieved
2026-11-04 development document (2.0-rc0) of the
[RISC-V ELF psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/).
