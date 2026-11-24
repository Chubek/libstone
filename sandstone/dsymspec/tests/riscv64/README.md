# RISC-V DSymspec checks

Run after generating the profile includes:

```sh
python3 sandstone/dsymspec/tests/riscv64/check.py build/dsymspec/dsymspec/generated
```

The test compiles a standalone, strict C11 consumer; checks the RV64GC and
RV32GC instruction checklists and RV64IM feature selection; compares generated
bytes against `llvm-mc` (LLVM 22); tests rejected operands; and compares branch
and jump relocation names, offsets and placeholder bytes with `llvm-readobj`.
For the additional extension inventory audit, pass `--opcodes` with a local
[`riscv-opcodes`](https://github.com/riscv/riscv-opcodes) checkout
(`cdd4192b1e0215a305f2f610a820fa43e6694290` was used).

## Scope and operand conventions

- RV64GC covers the canonical instruction mnemonics in I, M, A, F, D, C,
  Zicsr and Zifencei. A includes W/D and every ordering suffix; the same
  source models RV32GC with RV32 shifts, C.JAL and C+F, and without
  RV64-only load, word, atomic and floating conversion/move forms.
- RV64IM enables only I and M by default; it shares the RV64 instruction
  bank but consumers must check each form's `required` features. Encoder
  recipes are callable directly without feature enforcement. The optional
  scalar features modeled are Zba, Zbb, Zbs, Zbkb, Zknd, Zkne, Zknh, Zfh,
  Zfa, Zicond and Zihintpause. `priv` names the shared M/S subset (`mret`,
  `sret`, `wfi`, `sfence.vma`), not all privileged extensions.
- The RVV 1.0 bank includes configuration, register instructions, integer
  and FP arithmetic, mask operations, unit-stride/strided/indexed, segment,
  fault-only-first, mask, and whole-register loads/stores. Assembler aliases
  that expand to different instructions and non-1.0 vector extensions
  (including Zve subsets and Zv* crypto/FP) are outside this catalog. V
  register groups, SEW/LMUL legality, and dependence on CSR state cannot be
  validated by this stateless encoder; simple encoding constraints such as
  the mask register and explicit whole-register group alignment are checked.
  RV32 does not expose the 64-bit indexed forms, including indexed segments.
- Bindings are normalized. Memory takes the register, base register and
  byte displacement (atomic/vector base forms have no displacement).
  `fence` uses the architectural four-bit `iorw` sets. Rounded FP forms use
  the RM field numerically; `fli.{h,s,d}` takes the five-bit constant-table
  index, not a host floating-point value. V configuration uses the
  eight defined `vtype` bits, and masked V forms take a final `v0.t`
  (or `v0` for merge and carry) normalized to register zero.
- Branch and jump displacements are signed **byte offsets** and must be
  even. Symbol operands emit ELF branch/jump relocations, including the
  compressed variants. Layout, symbol expressions/addends, relaxation,
  relocations besides the explicitly declared records, instruction
  selection, alias expansion (`li` beyond the implemented immediate
  recipes), and object-file writing are client responsibilities.
- CSR instructions encode every 12-bit numerical address, 5-bit zimm and
  register class; common CSR names are provided but there is no complete
  register-name inventory for all optional privileged extensions. Read-only,
  absent or privileged CSR accesses can trap at execution, rather than
  constituting invalid instruction encodings.
- The C bank implements canonical forms and explicit `c.nop` hints. It
  does not treat every otherwise reserved/custom/hint bit pattern as an
  alias for the matching arithmetic mnemonic. `c.lui` uses the LLVM/GNU
  sign-extended 20-bit spelling.

Not claimed: unrelated ISA extensions (e.g. Q, H, Zc beyond legacy C, Zacas,
Zabha, Zbc, Zbkx, Zks, Zfhmin as an independently selectable profile), all
assembler pseudos or textual dialects, or exhaustive combinations of every
operand value. Optional assembler aliases are not a substitute for the
canonical instruction checklist.
