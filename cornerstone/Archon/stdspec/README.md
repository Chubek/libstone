# Archon standard specifications

These descriptions cover selected encodings, not complete implementations of
the ISAs.

| ISA | Entry point |
| --- | --- |
| RISC-V RV32I | [`riscv/rv32i.arch`](riscv/rv32i.arch) |
| RISC-V RV64I | [`riscv/rv64i.arch`](riscv/rv64i.arch) |
| Arm AArch64 | [`aarch64/root.arch`](aarch64/root.arch) |
| MIPS32 | [`mips32/root.arch`](mips32/root.arch) |
| WebAssembly 1.0 (Wasm32 core opcodes) | [`wasm/root.arch`](wasm/root.arch) |
| AMD64 (selected 64-bit register instructions) | [`amd64/root.arch`](amd64/root.arch) |
| JVM (selected bytecodes) | [`jvm/root.arch`](jvm/root.arch) |
| CLR (selected ECMA-335 CIL opcodes) | [`clr/root.arch`](clr/root.arch) |
| PDP-11 (selected 16-bit encodings) | [`pdp11/root.arch`](pdp11/root.arch) |

Instruction encodings and architectural effects are described in the `.arch`
sources, not hard-coded in the Archon runtime. See each directory for its
instruction coverage, assembly conventions, execution boundary, and golden
examples. WebAssembly, JVM, and CLR provide limited transactional stack
execution but are not module, class-file, or PE/CLI readers and do not attempt
to implement complete virtual machines. Where an instruction has no executable
semantics, neither the Python runtime nor the generated C99 emulator executes
it.
