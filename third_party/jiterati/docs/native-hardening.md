# Native artifact and optimization contracts

## Reference and scope

This refinement uses `lapislazuli/.info/binutils` in the enclosing libstone
checkout, dated `20111121` in `bfd/version.h`:

- `include/elf/external.h`: ELF32/64 headers, sections, symbols and programs;
- `include/elf/common.h`: file/section types, byte order and machine IDs;
- `bfd/elf64-x86-64.c`: relocation processing is required before execution;
- `gas/expr.c`: shift-count validation and unsigned shift intermediates.

The reference is not copied into the runtime and is not a library dependency.
When present, `JITERATI_BINUTILS_REFERENCE_DIR` enables compile-time external
layout comparisons in `binary_hardening`. AArch64/RISC-V/LoongArch constants
postdate this snapshot; the historical checkout does not establish their ISA
or ABI coverage. Existing `.agents/datasets/` remain the backend authority.

## ELF emission and linking

`asmlink::link_elf(target, bytes, base)` creates a **sectionless ET_EXEC**, not
ET_REL. Its payload must already be relocation-free process-entry machine
code. It fixes ELF64's 8-byte `e_shoff`, ELF32's program-field ordering,
PowerPC32's EM_PPC identity, and target byte order. Unknown/non-ELF targets,
empty code, unaligned bases and address-width overflow fail explicitly.
ISA extensions, ABI-specific flags, descriptors, startup code and runtime
libraries are not synthesized. A structurally valid file is not sufficient
to claim an executable ABI for every target; in particular PPC64 descriptors
and MIPS ABI flags remain caller responsibilities.

`native::link_executable(..., true)` accepts a validated, target-matching
ET_REL with a defined global `_start`. On Linux host targets it invokes the
system `cc -nostdlib -static -no-pie -Wl,-e,_start` linker route, retaining
sections and applying relocations. Undefined symbols and cross-target links
fail. It never substitutes extracted `.text` for object linking. With `false`,
the API delegates to the raw process-entry wrapper.

`AssembleReport::object_is_elf` distinguishes validated ET_REL from Keystone's
raw machine bytes. CLI `--emit obj/exe` requires ET_REL; assembler failure
does not produce a text fallback. A callable function is not automatically a
process entry: `--emit exe` requires `_start`, which must obey the OS startup
ABI and terminate explicitly. Existing output files survive these failures.

Keystone modes use the vendored API's ARM mode bit and explicit big-endian
PowerPC/SPARC/SystemZ modes; x86/i386 aliases select 32-bit mode. These mapping
checks do not imply that the optional Keystone library was built or executed.

The existing TAC backend still emits pseudo-assembly for some operations
(including parameterized AMD64 arithmetic). Such output fails assembly with
diagnostics; this refinement does not establish complete native TAC lowering.
CLI native execution regressions cover constant-return functions, while the
relocation regression assembles an explicit process-entry assembly fixture.

`extract_text_section` reads ELF32/64 in either byte order, including extended
section counts/indices. Bounds checks precede offset arithmetic. Extraction
rejects malformed tables/names, compressed/NOBITS text, duplicate `.text`,
oversized payloads, and nonempty REL/RELA sections targeting `.text`. It is
not a symbol loader, relocation applier, or proof of position independence.
Fully resolved absolute addressing and references to separately allocated
sections still require caller knowledge; only self-contained code may be
executed as an extracted payload.

## Bytecode and executable memory

- CLI compilation assembles each function separately. It no longer assigns
  the first function's code to every record, nor executes ELF headers when
  extraction fails. Unavailable native payloads remain IR-only records.
- Record decoding rejects truncated records, overflowing/invalid sizes,
  missing delimiters and invalid entry offsets instead of accepting a prefix.
- Legacy `ir_size code_size` record lines remain valid and byte-identical
  for offset-zero output. A nonzero `entry_offset` adds a third decimal field;
  upgraded readers preserve it. Older readers cannot execute this extension
  correctly and must not be used for nonzero-offset records.
- Execution requires matching canonical module/record/host targets, in-range
  i32 arguments and an entry offset within the code. The API still accepts
  only `i32()` and `i32(i32,i32)`; it does not verify arbitrary signatures.
- Memory transitions RW → RX, with instruction-cache publication, rather
  than remaining RWX. Native payloads remain **trusted code**, not a sandbox.
- Native assembly, linking, execution logs and archive compression use
  isolated temporary directories, removed by RAII on success or failure.

## Optimization

TAC folding normalizes i32/i64 operands to their declared width, uses unsigned
intermediates for wrapping add/sub/mul/neg/bit operations, explicitly sign
extends arithmetic shifts, and distinguishes signed/unsigned comparisons.
Divide-by-zero, signed MIN/-1 division/remainder, and invalid shift counts
remain unfolded operations rather than compiler-host undefined behavior.
Boolean results retain boolean immediates; mismatched/noninteger operands
are not folded. This is separate from ISA-specific shift masking.

CFG pruning detaches dead blocks' outgoing edges before destroying blocks,
removing dangling predecessor links. IR printing treats branch/phi labels,
allocation sizes, parameter indices and call symbols as metadata rather than
constant-pool references; constant-free control flow remains printable.

## Validation

```sh
cmake -S . -B /tmp/opencode/jiterati-check \
  -DJITERATI_BUILD_TESTS=ON -DJITERATI_BUILD_LUA=OFF -DJITERATI_BUILD_DOCS=OFF
cmake --build /tmp/opencode/jiterati-check --parallel
TMPDIR=/tmp/opencode ctest --test-dir /tmp/opencode/jiterati-check --output-on-failure
```

For UBSan, configure another build with
`-DCMAKE_CXX_FLAGS='-O1 -g -fsanitize=undefined -fno-sanitize-recover=all'`.
`binary_hardening` covers all class/endian layouts, extended numbering,
truncations, hostile offsets, target metadata, actual relocation-bearing
object linking, process execution, entry offsets and record rejection.
`tac_folding` covers width boundaries, wrapping, shifts and arithmetic traps.
`cli_native_artifacts` additionally checks fail-closed outputs and distinct
archived function entry points. `readelf`/`objdump` interoperability checks run
when those host tools are available. Cross-target execution is not tested.
