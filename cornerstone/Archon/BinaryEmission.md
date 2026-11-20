# RV64 binary emission boundary

The native backend returns bounded, NUL-terminated GAS assembly for
little-endian RV64I or RV64IM and the integer LP64 ABI by default. A separate mode encodes
raw instruction bytes for a single function. The generated C API does not
invoke tools, assemble ELF objects, link executables, or execute generated code.

- `.option norvc` and `.option norelax` keep instruction widths and paired
  address sequences fixed while assembling this source.
- RV64I assembly specifies the `rv64i2p1` ELF ISA attribute and selects
  `.option arch, rv64i` inside the pushed option region. Use a toolchain
  that supports these directives; no M opcode is emitted.
- Conditional transfers use an inverted short branch over a two-instruction
  `auipc`/`jalr` jump. Its skip is always 12 bytes. The jump uses reserved `x28`
  and `%pcrel_lo` references the local anchor on `auipc`. All such destinations
  are in the same bounded `.text` section, well within the pair's range.
- `call` and `lla` name validated symbols, not assembler fragments. A static
  link resolves call relocations (`R_RISCV_CALL` or `R_RISCV_CALL_PLT`) and
  address pairs (`R_RISCV_PCREL_HI20` / `R_RISCV_PCREL_LO12_I`). `lla` is a
  direct PC-relative materialization, not a GOT lookup. The link must keep
  caller/target and text/data within range. In the default mode,
  shared-library symbol preemption and PIC/GOT are not provided. TLS, unwind
  metadata, startup files, and a runtime are not provided in either mode.
- Functions are exported ELF function symbols. Defined globals have 8-byte
  alignment and size metadata, using `.data` for explicit words and `.bss` for
  zero initialization. External data has no definition in the generated file.

### PIC assembly

`ARCHON_BACKEND_PIC` (driver option `--pic`) is an assembly-only alternative.
It preserves the default ABI and instruction lowering, adds `.option pic`, and
uses GAS `la` rather than `lla` for both data and function addresses. For
preemptible external symbols, the assembler can therefore emit GOT-relative
materialization suitable for a shared link. Calls remain assembler `call`
forms and can use PLT relocations. Physical-record and raw-byte modes reject
the PIC flag because they do not model external GOT relocations.

This flag emits linkable PIC assembly; it does not itself assemble an object,
produce a shared library, define symbol visibility, or provide a dynamic
loader. The existing default output is unchanged.

## Raw instruction bytes

Use `ARCHON_BACKEND_MACHINE_CODE` with `archon_backend_compile_ex` (or
`archon_backend_compile_text_ex`), or run:

```sh
build/archon-backend --input function.mir --emit-code > function.bin
```

The module must define exactly one function and no imports or globals. Its
entry point is byte zero; local branches, self-calls, and its own function
address are resolved with checked PC-relative offsets. Instructions are
four-byte little-endian words. The artifact size is a byte count, **not**
the length of a C string; valid instruction bytes can contain NUL. This mode
does not produce ELF, external relocations, executable memory, or a JIT.

The binary encoder uses the same reviewed target forms that qualify operands
for GAS and physical records, including signed and unsigned 8/16/32-bit loads,
8/16/32-bit stores, and full-width integer or pointer transfers. A layout
pass checks duplicate/unresolved local labels and transfer ranges in all three
output modes. Tests compare raw bytes with independently assembled and linked
GAS and execute recursive raw code at different addresses in an emulator.

## Producing an ELF with external tools

For example, with LLVM tools, where `startup.o` supplies `_start` and any
imports are resolved by the provided objects:

```sh
llvm-mc -triple=riscv64-unknown-linux-gnu -mattr=+m -filetype=obj \
    -o module.o module.s
ld.lld -m elf64lriscv -static --no-relax --fatal-warnings \
    -e _start -o program.elf startup.o module.o
```

Alternatively, Clang's integrated assembler accepts
`--target=riscv64-unknown-linux-gnu -march=rv64im -mabi=lp64 -c`
(use `-march=rv64i` for the RV64I profile).
The assembly uses GNU RV64 syntax; a RISC-V GNU assembler can be selected
explicitly with `-march=rv64im -mabi=lp64`. When wrapping these tools, pass
individual argv elements, not a constructed command string.

The optional Python bridge compiles a runtime text program, invokes the
configured assembler with an argument vector, checks the resulting ELF
header and section geometry, and atomically publishes the object. Linking is
a separate step that takes already-produced objects:

```sh
python3 -m archon native-object program.mir --backend build/archon-backend \
    --assembler clang -o program.o
python3 -m archon native-link --linker ld.lld --entry _start \
    -o program.elf startup.o program.o
```

Use `--assembler-flavor gnu` with a RISC-V GNU assembler. The backend,
assembler, and linker are trusted host executables; the native C library
never runs them automatically.

`archon.irgen.objectwriter.link_executable(linker, objects, entry, output)`
requires an explicit, trusted LLVM ELF LLD (`ld.lld`). GNU `ld` is not
supported. Each input must be a regular little-endian RISC-V ELF64
relocatable object (`ET_REL`) with zero RISC-V ELF flags: the soft-float
LP64 ABI, without RVC or RVE flags. The bridge accepts System V and GNU ELF
OS ABI markers with ABI version zero. The check does not certify the instruction
set used inside an object. For a binary runnable without M, assemble all
inputs for `rv64i` and `lp64`; do not link in RV64IM-only code.
There is no support for archives, linker scripts, or implicit libraries.
The caller supplies a defined entry symbol, startup code, and any imports.

The bridge snapshots at most 256 input objects totaling at most 256 MiB
into a private directory, verifies section geometry, and passes only the
validated copies to LLD. It disables relaxation and input-file library
directives, and rejects linker warnings so a missing entry cannot silently
fall back. It checks that the staged result is a matching non-PIE `ET_EXEC`
with no interpreter or dynamic segment, no writable-executable segments or
executable stack, bounded load ranges (at most 256 MiB in total), and a
four-byte-aligned entry backed by
executable file bytes. The destination is replaced atomically after validation;
an error leaves an existing destination untouched and does not create a partial
output. The bridge does not execute the result.

The link tests build two Archon objects and a separate freestanding RV64 C
fixture, verify the resolved entry and call/address instructions, and run the
image when Unicorn with RV64 support is available. The broader end-to-end suite
also supports QEMU. GNU `as` was not used in this verification.

## Sandstone and Yellowstone compatibility

- [Sandstone's C assembler SDK](../../sandstone/assembler/assembler.h)
  provides `sandstone_asm_new`, `sandstone_asm_assemble_string`, section/symbol/
  relocation getters, and `sandstone_asm_write`. The write formats are flat
  `bin` and `obj` (Sandstone JSON), not ELF. Its flat output rejects unresolved
  instruction-field fixups, and its JSON is not a linker input accepted by
   Yellowstone. ArchML `.insn` also does not accept symbolic relocations.
   See the [Sandstone assembler documentation](../../sandstone/docs/assembler.md).
- [Yellowstone qBFD / ArchBFD](../../yellowstone/qbfd/ArchBFD.md) provides
   object, symbol, section and relocation inspection, not a relocatable ELF
   writer or relocation application API. Its in-place strip support is not
   an object writer. To cross-check Archon's output with the existing reader,
   build `archbfd-inspect` with `-DARCHBFD_BUILD_EXAMPLES=ON` and set
   `ARCHON_QBFD_INSPECT` to that executable when running
   `python3 -m unittest discover -s tests -p test_native_link.py`.
- [Yellowstone's C linker SDK](../../yellowstone/ld/ld.h) provides
   `qld_link_elf` and `qld_link_elf_bytes`. A null linker uses the native ELF
   backend. The [native relocation applier](../../yellowstone/ld/relocate.cpp)
   implements simple RISC-V data relocations but does not implement the call
   and paired instruction-field relocations above. Native link probes with
   Archon output and its startup object failed on `R_RISCV_CALL_PLT` (type 19)
   and `R_RISCV_PCREL_HI20` (type 23). Recognizing relocation metadata in
   qBFD does not apply it.
   An explicit external ELF linker is an optional Yellowstone build feature,
   not a native implementation or an Archon dependency.

There is no native Sandstone-to-Yellowstone route for this output here, and
no new object format or partial relocation writer is supplied.
