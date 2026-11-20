# DEC PDP-11 register-direct subset

[`root.arch`](root.arch) assembles and disassembles the following base PDP-11
instructions, as found on the PDP-11/20. Every encoding is one 16-bit word,
with the low byte first in memory. Word and byte data operations both use
one instruction word in register-direct mode.

| Form | Supported mnemonics |
| --- | --- |
| Fixed system and trap words | `halt`, `wait`, `rti`, `bpt`, `iot`, `reset` |
| Fixed condition-code words | `nop`, `clc`, `clv`, `clz`, `cln`, `ccc`, `sec`, `sev`, `sez`, `sen`, `scc` |
| One register, word | `swab`, `clr`, `com`, `inc`, `dec`, `neg`, `adc`, `sbc`, `tst`, `ror`, `rol`, `asr`, `asl` |
| One register, byte | `clrb`, `comb`, `incb`, `decb`, `negb`, `adcb`, `sbcb`, `tstb`, `rorb`, `rolb`, `asrb`, `aslb` |
| Two registers, word | `mov`, `cmp`, `bit`, `bic`, `bis`, `add`, `sub` |
| Two registers, byte | `movb`, `cmpb`, `bitb`, `bicb`, `bisb` |

Assembly is lowercase with DEC source-first operand order (`mov r1, r2`).
`r0` through `r7` have register numbers 0 through 7; `r0` is writable, not
hardwired to zero. `sp` and `pc` are accepted aliases for `r6` and `r7`;
disassembly always prints numbered registers. The mode bits are zero:
the destination register is in bits 2–0 and the source register, when
present, is in bits 8–6.

No memory, deferred, indexed, or immediate forms are included. Branches and
subroutine instructions are omitted, including `JMP` and `JSR`, for which
mode 0 is illegal. Optional and later-model instructions (such as EIS and
`RTT`) are not included. Only the selected fixed condition-code words are
accepted: combined masks other than `ccc` and `scc`, and the alternate
no-operation word `000260` (octal), are outside this subset.

## ABI coverage

[`abi_facts_v2 UnixV7Scalar`](root.arch) describes the Research Unix V7 C
convention for word-sized integers and pointers, not an ISA-wide calling
convention. `JSR PC` pushes the return PC at `0(sp)`; `RTS PC` pops it.
The stack grows down and is word-aligned (`sp % 2 == 0`). There are no
register arguments: the first word argument is at `2(sp)` on entry, and a
word result is in `r0`. [V7 `setjmp`][v7setjmp] shows the argument load,
the saved return PC and the result in `r0`.

`r0` and `r1` are scratch; `r2`–`r5` and `sp` are preserved across calls.
The profile reserves the C frame pointer `r5` (see V7 [`csv`/`cret`][v7csv]),
`sp`, and `r7` (current PC, not a return-address register).
The profile does not model results or arguments spanning multiple words,
non-C `JSR` linkage, or a native backend. This subset does not encode or
execute either `JSR` or `RTS`.

## Execution limits

All register-direct data instructions, the selected condition-code
instructions, and canonical `nop` (octal `000240`) have ArchSem. The four
low processor-status-word condition codes are declared as one-bit scalar
state named `N`, `Z`, `V`, and `C`.

The semantics model the PDP-11 rules used by this subset:

- Word operations update all 16 bits of a register.
- Byte destinations preserve bits 15–8. Register-destination `movb` is the
  architectural exception: it sign-extends bit 7 through bits 15–8.
- `cmp`/`cmpb` compute source minus destination; `sub` computes destination
  minus source. For subtraction, `C` records an unsigned borrow.
- `mov`, `bit`, `bic`, and `bis` (and their byte forms) clear `V` but preserve
  `C`; `inc` and `dec` also preserve `C`.
- `swab` derives `N` and `Z` from the low byte after swapping and clears
  `V` and `C`.

The six fixed system/trap words remain deliberately encoding-only:

| Instructions | Missing architectural behavior |
| --- | --- |
| `halt`, `wait` | Processor run state, privilege checks, and interrupt arbitration |
| `bpt`, `iot` | Trap-vector entry and complete saved processor state |
| `reset` | I/O-bus and device reset effects |
| `rti` | Stack-memory reads plus restoration of the complete PSW and execution mode |

Executing one of those words reports `no executable semantics`; it never
pretends the instruction is a no-op.

This package declares a 64 KiB `MainMemory`, but the mode-zero instructions
in this subset never access it. Archon also does not model instruction fetch,
implicit normal PC advance, or trace traps. An explicit register-direct write
to `r7`/`pc` updates that register slot, while program execution itself remains
linear. The tests use full machine state to verify flags, unchanged memory,
input immutability, aliases, same-register operands, and transactional failure.

From `cornerstone/Archon`:

```sh
python3 -m archon compile stdspec/pdp11/root.arch -o pdp11.archdb
python3 -m archon asm pdp11.archdb 'mov sp, pc'  # 8711
python3 -m archon dis pdp11.archdb 8711          # mov r6, r7
python3 -m archon run pdp11.archdb 'inc r0' --reg r0=32767
python3 -m unittest discover -s stdspec/pdp11 -v
```

## Golden encodings

[`encoding-golden.tsv`](encoding-golden.tsv) has two tab-separated columns:
hex bytes in memory order and canonical assembly. The vectors were
cross-checked against DEC's [PDP-11/20 Handbook][dec20] (pp. 13, 19–23,
§4.8, and Appendix A) and the [PDP-11/40 Processor Handbook][dec40]
(Appendix D, “Numerical Op Code List”). SIMH V3.12-5's PDP-11 symbolic
assembler and disassembler also agreed on every vector with the CPU set to
`11/20`. For example, `deposit -m 1000 MOV R1,R2` followed by `examine 1000`
prints octal `010102`, which is `42 10` in memory order. SIMH prints `SP`
and `PC` where Archon prints `r6` and `r7`.

The ArchSem register and condition-code behavior was separately checked
against the instruction descriptions in those handbooks and SIMH's PDP-11
CPU implementation. The Python tests compute expected results with an
independent table-driven reference model rather than restating the SemIR.

[dec20]: https://bitsavers.trailing-edge.com/pdf/dec/pdp11/handbooks/PDP-1120_Handbook_1972.pdf
[dec40]: https://bitsavers.trailing-edge.com/pdf/dec/pdp11/handbooks/PDP-11_40_Processor_Handbook_1972.pdf
[v7setjmp]: https://github.com/dspinellis/unix-history-repo/blob/Research-V7/usr/src/libc/gen/setjmp.s
[v7csv]: https://github.com/dspinellis/unix-history-repo/blob/Research-V7/usr/src/libc/crt/csv.s
