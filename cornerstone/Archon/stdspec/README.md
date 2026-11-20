# Archon standard specifications

These descriptions cover selected encodings, not complete implementations of
the ISAs.

| ISA | Entry point | Declarative scalar ABI coverage |
| --- | --- | --- |
| RISC-V RV32I | [`riscv/rv32i.arch`](riscv/rv32i.arch) | v1: [ILP32 scalar integer profile](riscv/README.md#integer-abi-facts) |
| RISC-V RV64I | [`riscv/rv64i.arch`](riscv/rv64i.arch) | v1: [LP64 scalar integer profile](riscv/README.md#integer-abi-facts) |
| RISC-V RV64IM | [`riscv/rv64im.arch`](riscv/rv64im.arch) | v1: LP64 scalar integer profile |
| Arm AArch64 | [`aarch64/root.arch`](aarch64/root.arch) | v2: [AAPCS64 integer profile](aarch64/README.md#calling-convention), with SP distinct from XZR. |
| MIPS32 | [`mips32/root.arch`](mips32/root.arch) | v1: [O32 non-PIC scalar integer profile (`O32NonPIC`)](mips32/README.md#scalar-non-pic-o32-abi-facts) |
| WebAssembly 1.0 (Wasm32 core opcodes) | [`wasm/root.arch`](wasm/root.arch) | Absent: [typed operand stack and function frames](wasm/README.md#abi-coverage), not a register ABI. |
| AMD64 (selected 64-bit register instructions) | [`amd64/root.arch`](amd64/root.arch) | v2: [SysV AMD64 LP64 integer profile](amd64/README.md#calling-convention), with a stack-resident return PC. |
| JVM (selected bytecodes) | [`jvm/root.arch`](jvm/root.arch) | Absent: [method locals and operand stacks](jvm/README.md#abi-coverage), not a register ABI. |
| CLR (selected ECMA-335 CIL opcodes) | [`clr/root.arch`](clr/root.arch) | Absent: [managed argument slots and evaluation stack](clr/README.md#abi-coverage), not a register ABI. |
| PDP-11 (selected 16-bit encodings) | [`pdp11/root.arch`](pdp11/root.arch) | v2: [Research Unix V7 C word-scalar profile](pdp11/README.md#abi-coverage), not an ISA-wide convention. |

[Scalar ABI facts](../Language.md#scalar-abi-facts) are optional metadata, not a
complete ABI or a guarantee of native code generation. Version 2 still describes
single-register integer results, but allows a separate scalar stack pointer or a
stack-resident return address. Neither version models managed calls or typed
frames. The native providers independently qualify the reviewed
[RV64I and RV64IM LP64 profiles](riscv/README.md#rv64-native-profiles) and the
[SysV AMD64 and AAPCS64 scalar GAS profiles](../NativeScalarGAS.md).

Instruction encodings and architectural effects are described in the `.arch`
sources, not hard-coded in the Archon runtime. See each directory for its
instruction coverage, assembly conventions, execution boundary, and golden
examples. WebAssembly, JVM, and CLR provide limited transactional stack
execution but are not module, class-file, or PE/CLI readers and do not attempt
to implement complete virtual machines. Where an instruction has no executable
semantics, neither the Python runtime nor the generated C99 emulator executes
it.
