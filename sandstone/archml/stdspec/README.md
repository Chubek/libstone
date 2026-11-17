# ArchML compiler profiles

Load these version-1 S-expression descriptions using `mulib_archml_load`.
The canonical RV64 RISC-V filename is **`riscv64.aml`**.
The sixteen additional profiles use the requested **`.am`** filenames;
the loader does not depend on the filename extension.

| File | Default compiler profile | Concrete forms |
|---|---|---:|
| `amd64.aml` | Long-mode little-endian, SysV AMD64, integer + SSE/SSE2 | 124 |
| `arm64.aml` | A64 little-endian EL0, AAPCS64, integer + scalar FP/selected NEON | 80 |
| `riscv64.aml` | RV64 little-endian LP64D, I/M/A plus selected D/C and FENCE.I | 92 |
| `ppc64.aml` | Big-endian PowerPC64, selected non-recording integer forms | 23 |
| `mips.aml` | Big-endian pre-R6 MIPS32, selected integer forms and delayed branches | 29 |
| `avr32.aml` | Big-endian AVR32A, selected 16-bit forms; **not AVR8** | 9 |
| `wasm.aml` | WebAssembly MVP wasm32, selected stack operations and SLEB7 constants | 36 |
| `amdgpu.am` | GCN gfx600 wave64, ordinary-SGPR SALU and wave termination | 8 |
| `arm.am` | Little-endian ARMv7 A32, AL-only integer subset; not Thumb/AArch64 | 19 |
| `nvptx.am` | NVPTX64 virtual scalar integer ISA, PTX text only | 0 (15 operations) |
| `spirv.am` | Selected little-endian SPIR-V word sequences with nonzero IDs | 12 |
| `xtensa.am` | Little-endian Xtensa 24-bit integer and memory forms | 20 |
| `bpf.am` | Little-endian eBPF ALU32/64, LDDW, memory, short branches and EXIT | 57 |
| `directx.am` | DXIL scalar i32 min/max and dispatch/group IDs, LLVM IR only | 0 (8 operations) |
| `m68k.am` | Big-endian 68000 register-direct byte/word/long integer subset | 20 |
| `hexagon.am` | Hexagon V60 scalar forms in single-instruction packets | 7 |
| `dex.am` | DEX035 selected virtual-register integer/control/invoke forms; ART metadata | 98 |
| `loongarch64.am` | Little-endian LA64 scalar integer, memory and control subset | 20 |
| `avr8.am` | Classic AVR 8-bit register ALU and high-register immediates | 11 |
| `msp430.am` | MSP430 16-bit register-direct ALU, JMP and NOP | 9 |
| `sparc.am` | Big-endian SPARC V8 visible-window ALU and delayed BA subset | 13 |
| `systemz.am` | z/Architecture 64-bit RRE ALU, LGHI and NOPR subset | 8 |
| `evm.am` | EVM 256-bit stack operations, bounded PUSH, DUP and SWAP forms | 69 |

The AMD64, AArch64 and RV64 compiler profiles include register storage/aliases, classes and allocation restrictions,
concrete bit encodings, structured P-code, memory/atomic/exception semantics,
vectors/predicates, features/modes, types/legalization, instruction selection and
scheduling metadata, rewrites, reachability/relocations, assembly syntax, CPU
extension descriptions and SMT proof obligations. ABI and object-layout records
provide additional compiler integration data. Microarchitecture costs are
conservative generic estimates, not measured silicon timing guarantees.

The profiles beyond AMD64/AArch64/RV64 are deliberately smaller subsets, not
full compiler-backend profiles. They make no ABI, tuning or exhaustive ISA claims.
MIPS branches explicitly execute a delay slot, unlike RISC-V branches. PPC64 uses
constant zero for an encoded r0 base in ADDI/ADDIS and supported memory forms,
but reads the actual r0 register in ordinary arithmetic/logical forms. AVR32
excludes PC operands and preserves the specified status flags. WebAssembly has
no invented physical registers: operations consume/produce typed stack values.
Its `i32.const-sleb7`/`i64.const-sleb7` forms accept only -64..63; the current
fixed-bit encoding schema cannot express general variable-length LEB128.

The additional profiles preserve important target distinctions:

* AMDGPU fixes **gfx600**, excludes special/inline/literal operand encodings, and
  models SCC effects; SALU runs once per wave, not once per active lane. GFX10+
  wave32 is recorded as metadata only and does not change this wave64 profile.
* ARM fixes A32 condition AL and S=0. PC register operands are excluded; B/BL
  use PC+8 as their displacement base, and BL writes PC+4 to LR.
* Xtensa excludes density/window rotation; its offsets are unsigned scaled bytes.
* eBPF excludes writes to r10, zero-extends ALU32 results and encodes LDDW as two
  slots. Branch displacements are **bytes**, relative to PC+8. Encoding is not
  verifier approval; pointer provenance and branch-slot validity remain separate.
* M68k short data-register writes preserve upper bits and model CCR/X effects.
  Hexagon words fix packet-end parse bits; they cannot be repacked without a
  packetizer and hazard checks.
* SPIR-V operands are module IDs, not physical registers. The encoder checks
  word templates and nonzero IDs, not types, capabilities, header bounds or CFGs.
  `OpUnreachable` denotes undefined execution, not a guaranteed hardware trap.
* NVPTX and DirectX use validated **`operation`** records with `value` operands
  and P-code. They have no invented machine opcodes or finite physical register
  banks. Query these records with `mulib_archml_find(doc, "operation", name)`;
  `encode`/`decode` return `MULIB_ARCHML_INVALID` without changing outputs.
  Consumers emit the PTX text/DXIL LLVM IR templates and supply valid module
  context. A DXIL opcode is an IR call argument, not a byte instruction encoding.
* DEX uses 16-bit little-endian code units and **virtual frame indices**, not a
  finite physical-register bank. Register indices are encoded as immediate
  operands and accessed by dex-read/dex-write P-code. Wide pairs may overlap and
  need no even alignment. Branch values are byte displacements from current PC,
  encoded at scale 2. Verification, pool lookup and invoke-result adjacency are
  runtime obligations. ART GC/JIT/AOT records are explicitly metadata-only;
  MuLIB's executable integer-method subset is documented in mulib-art.h.
* LoongArch fixes LA64 and models r0 discard writes, masked variable shifts,
  naturally aligned LD.D/ST.D, PC-relative branches without delay slots, BL's
  link write and JIRL's pre-state source/link behavior.
* AVR8 is distinct from `avr32.aml`. Its FRdRr register fields are five bits,
  while FRdK uses an encoded 0..15 view of r16..r31. MSP430 ordinary operands
  intentionally exclude r0..r3 so PC, SR and constant-generator interpretations
  cannot be selected accidentally.
* SPARC uses the current register-window view and gives BA an explicit delay
  slot; SAVE/RESTORE cannot be inferred from the flat aliases. SystemZ preserves
  variable two/four-byte instruction lengths and models condition-code effects.
* EVM has no physical registers. Stack words are 256 bits and depth is capped at
  1024. PUSH immediates are big-endian; ArchML v1's scalar encoder API limits
  concrete forms to PUSH1..PUSH8 (plus fork-gated PUSH0). Gas/fork checks and
  state rollback remain execution-environment obligations.

## Local source provenance

Reviewed facts come from `/mnt/lambda/ArchTk/infobank/` (paths below are relative
to that directory); regeneration does not require the corpus:

| Profile | Source files |
|---|---|
| RISC-V | `Capstone-Arch/RISCV/RISCVGenDisassemblerTables.inc`; `LLVM-Target/RISCV/RISCVInstrInfo.td` |
| PowerPC64 | `Capstone-Arch/PowerPC/PPCGenDisassemblerTables.inc`; `LLVM-Target/PowerPC/PPCInstrInfo.td`, `PPCInstr64Bit.td`, `PPCInstrFormats.td` |
| MIPS32 | `Capstone-Arch/Mips/MipsGenDisassemblerTables.inc`; `LLVM-Target/Mips/MipsInstrInfo.td`, `MipsInstrFormats.td` |
| AVR32A | `Ghidra-CPU/Atmel/data/languages/avr32a.slaspec` and its arithmetic, logic, data-transfer and system-control `.sinc` files |
| WebAssembly | `Capstone-Arch/WASM/WASMDisassembler.c`; `Binaryen-WASM/wasm-stack.cpp`, `wasm-validator.cpp`, `wasm.cpp` |
| AMDGPU | `LLVM-Target/AMDGPU/SOPInstructions.td`, `SIRegisterInfo.td`, `GCNSubtarget.h` (gfx6 forms and wave-size distinction) |
| ARM A32 | `LLVM-Target/ARM/ARMInstrInfo.td`, `ARMInstrFormats.td`, `ARMRegisterInfo.td` |
| NVPTX | `LLVM-Target/NVPTX/NVPTXInstrInfo.td`, `NVPTXInstrFormats.td`, `NVPTXRegisterInfo.td` |
| SPIR-V | `LLVM-Target/SPIRV/SPIRVInstrInfo.td`, `SPIRVInstrFormats.td`, `MCTargetDesc/SPIRVMCCodeEmitter.cpp` |
| Xtensa | `LLVM-Target/Xtensa/XtensaInstrInfo.td`, `XtensaInstrFormats.td`, `XtensaRegisterInfo.td` |
| eBPF | `LLVM-Target/BPF/BPFInstrFormats.td`, `BPFInstrInfo.td`, `BPFRegisterInfo.td`, `MCTargetDesc/BPFMCCodeEmitter.cpp` |
| DirectX | `LLVM-Target/DirectX/DXIL.td`, `DXILOpBuilder.cpp`, `DXILWriter/DXILBitcodeWriter.cpp` |
| M68k | `LLVM-Target/M68k/M68kInstrArithmetic.td`, `M68kInstrData.td`, `M68kInstrControl.td`, `M68kRegisterInfo.td` |
| Hexagon | `LLVM-Target/Hexagon/HexagonDepInstrInfo.td`, `HexagonDepInstrFormats.td`, `HexagonInstrFormats.td`, `MCTargetDesc/HexagonBaseInfo.h` |
| LoongArch64 | `LLVM-Target/LoongArch/LoongArchInstrInfo.td`, `LoongArchInstrFormats.td`, `LoongArchRegisterInfo.td` |
| AVR8 | `LLVM-Target/AVR/AVRInstrInfo.td`, `AVRInstrFormats.td`, `AVRRegisterInfo.td`, `AVRDevices.td` |
| MSP430 | `LLVM-Target/MSP430/MSP430InstrInfo.td`, `MSP430InstrFormats.td`, `MSP430RegisterInfo.td` |
| SPARC V8 | `LLVM-Target/Sparc/SparcInstrInfo.td`, `SparcInstrFormats.td`, `SparcRegisterInfo.td` |
| SystemZ | `LLVM-Target/SystemZ/SystemZInstrInfo.td`, `SystemZInstrFormats.td`, `SystemZRegisterInfo.td` |
| EVM | `Capstone-Arch/EVM/EVMDisassembler.c`, `EVMMappingInsn.inc`; `Radare2-Opcodes/evm/evm.c`, `evm.h` |

DEX/ART facts come from repository-local `standards/android/android-bytecode.txt`,
`android-dex.txt`, `android-art.txt` and `android-abi.txt`; its original generator
tables do not require any third-party source tree.

AVR32 is absent from this Capstone snapshot; its profile uses the surrounding
Ghidra corpus instead. Tables describe selected facts, not a wholesale conversion
of third-party source. LLVM MC independently checks the supported PowerPC64,
MIPS, ARM A32, gfx600 SALU, eBPF, Hexagon, LoongArch64, AVR8, MSP430,
SPARC and SystemZ forms. SPIRV-Tools independently checks SPIR-V forms when
`spirv-as` is installed. AVR32, WebAssembly, Xtensa, M68k and EVM have
source-derived byte fixtures instead. The local LLVM MC build has no
Xtensa/M68k/EVM targets or SPIR-V text parser.

## Coverage contract

An `instruction` record is one encodable form, with bounded operands and a P-code
expression. Unsupported addressing variants are not implied by the mnemonic.
An `operation` record has no binary encoding and cannot be passed to the encoder.
Encoder support is intentionally explicit: the files do **not** contain every
instruction, privileged facility or extension of these very large architectures.
For example, AVX/AVX512, SVE/SVE2 and RVV have feature/state descriptions but are
marked metadata-only, and their encodings cannot be selected from these profiles.
RISC-V F/D coverage is partial even though LP64D ABI data is provided. Both x86
Intel and AT&T syntax contracts are present; there is no built-in text assembler.

Legalization records describe promotion, expansion, scalarization and target
runtime fallbacks. A compiler still implements ABI classification, feature/mode
selection, instruction selection, allocation, scheduling, object emission,
relocation handling, runtime linkage and semantic verification. Scalable operations
without an implemented backend must be diagnosed. These assets are a substantial
backend data foundation, not a claim of a complete production compiler target.

## Using the data

1. Select the target/ABI and an explicitly enabled feature set; validate OS state.
2. Legalize IR types and operations according to available forms and runtime ABI.
3. Match `selection` records; inspect `operands`, implicit uses/defs and P-code.
4. Allocate physical views using storage overlap, classes, ties and ABI constraints.
5. Schedule effects and dependencies with the selected tuning model.
6. Lay out code, check displacement scale/range, insert veneers and relocations.
7. Call the concrete encoder with architectural register numbers and byte-valued
   signed displacements, or translate assembly templates into the chosen dialect.
8. Apply machine rewrites only after discharging their flags/memory/trap guards.

The public schema and ownership contracts are in
[`manual/mulib-archml.md`](../../manual/mulib-archml.md).
The AML is generated deterministically from
[`tools/generate_archml.py`](../../tools/generate_archml.py),
[`tools/archml_extra.py`](../../tools/archml_extra.py) and
[`tools/archml_targets.py`](../../tools/archml_targets.py), and
[`tools/archml_android.py`](../../tools/archml_android.py); use `--check` in CI.
Tests load these actual files, roundtrip every form, and compare representative
forms with independent LLVM MC encodings.
