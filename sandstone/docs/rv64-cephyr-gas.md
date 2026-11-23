# Assembling Cephyr RV64IM output

`rv64im.dsymspec` selects the LP64 soft-float ABI and defaults to I+M.
`riscv64.dsymspec` shares the integer encoders but selects LP64D with additional
ISA features. **Use the RV64IM bank for Cephyr**, including `long double`.
Binary128 arithmetic calls software helpers; it does not imply Q, D or F
instructions or a hard-float ELF header.

The RV source adapter normalizes syntax and expands pseudos; it emits no
instruction bytes. The selected SymAS bank supplies opcodes, operand schemas,
feature predicates and relocation kinds. The common parser and two-pass
assembler lay out the resulting instructions. No LLVM assembler or encoder is
used.

* Covers the physical integer ALU, shifts, comparisons, signed/unsigned
  multiply/divide/remainder operations used by Cephyr; byte/half/word/double
  loads and stores including zero-extending loads; direct and indirect
  control flow. Integer memory operands use signed 12-bit `offset(base)`.
  Register ABI aliases include the high temporaries and saved registers.
* `li` accepts full 64-bit decimal/hex bit patterns and expands to ADDI/SLLI;
  the sequence is deterministic, not necessarily shortest. `mv`, `neg`, `j`,
  `jr`, one-register `jalr`, `ret`, `beqz` and `bnez` are supported.
* `call` uses AUIPC/JALR with `R_RISCV_CALL_PLT` on the first instruction.
  `lla` and non-PIC `la` use PCREL_HI20 + PCREL_LO12_I; PIC `la` uses
  GOT_HI20 + PCREL_LO12_I with `ld`. Each low relocation refers to the
  high instruction's anchor. Explicit Cephyr-style PC-relative AUIPC/JALR
  pairs, repeated single-digit `Nf`/`Nb` branch labels, branch/JAL relocations
  and ELF32/64-width absolute data references are supported.
* `.option push/pop`, `pic/nopic`, `norvc`, `norelax`, and `arch,rv64i` /
  `arch,rv64im` apply within the source. A source directive cannot enable
  unavailable ISA features. No implicit compressed encoding or optional RELAX
  relocation is generated. The ELF ABI comes from the bank, not the directive.
* Cephyr's private `.set` call aliases can target a defined function or an
  external function. Defined aliases retain the target address, function type
  and size. External references relocate against the actual external name.

## Validation

```sh
cmake -S sandstone -B "$B" -DSANDSTONE_BUILD_TESTS=ON
cmake --build "$B" --target sandstone-as sandstone-spec-compiler
ctest --test-dir "$B" -R 'sandstone_(rv_gas|spec_pipeline)' --output-on-failure
python3 sandstone/tests/rv_gas.py --build-dir "$B" --cephyr "$CEPHYR" \
  --runtime "$CEPHYR_RV64_PACKAGE" --qemu "$QEMU_RISCV64" --qobjld "$QOBJLD"
```

The base test decodes ELF64 section bytes, relocation offsets/types, anchor
values and ABI flags; rejects unavailable instructions, invalid ranges and
register classes; and independently evaluates full-width `li` sequences.
The optional test assembles PIC **and** non-PIC GAS from the real Cephyr/GCC
binary128 interoperability suite (fixed, split, stacked and variadic arguments,
arithmetic, callbacks and aggregate returns). `--runtime` selects the directory
containing compatible `crt0.o` and `libc.a`; put the GNU cross tools on PATH or
set `--cross-prefix`. It checks final linked `.text` against GNU as, and
runs both on `qemu-riscv64 -cpu rv64i,m=true`. With `--qobjld` it also runs a
separate qobjld static link of the SandAS object. GNU tools are test oracles,
not SandAS dependencies.

**Boundaries:** this is coverage of Cephyr's integer GAS dialect, not complete
GNU RISC-V syntax, a complete RV64GCV catalog, ELF attribute synthesis, TLS
relocations, general `.set` symbol aliasing, or a relaxer. Local ELF branch
and high/low relocations may remain in the object even where GAS resolves
them early: compare linked code, not raw object equality. Flat output cannot
apply unresolved instruction-field relocations. Dynamic linking, linker
relaxation, out-of-range branches and other linker policies are separate from
the assembler's relocatable-object support.
