# PCC native lowering: x86-64 SysV Linux

`Native.hpp` targets the Linux x86-64 ELF SysV LP64 calling convention. It
supports a single verified, straight-line Novostone IR block with zero to six
i32 arguments and one i32 return. Supported IR operations are `Param`, `Const`,
`Copy`, modular `Add`/`Sub`/`Mul`, `Eq`, unsigned `Ult`, and `Select` with a
nonzero condition. IR memory, control flow, phi nodes, stack-passed arguments,
varargs, vectors, and other targets are not supported.

```cpp
#include <novostone/PCC/Native.hpp>

// input is the verified IR function to bind. If it is an optimizer result,
// independently verify the source-to-optimized-IR proof first.
novostone::pcc::NativeRequest request;
request.symbol = "my_i32_function";
auto lowered = novostone::pcc::lower_native(input, request);
if (lowered.program) {
  auto checked = novostone::pcc::verify_native(input, *lowered.program, request);
  if (checked.status == novostone::pcc::NativeStatus::Proved) {
    // Consumers may assemble the exact string checked. They trust the
    // assembler, linker, loader, and machine to execute its stated semantics.
    save_assembly(checked.assembly);
  }
}
```

`lower_native` produces a candidate, not a verdict. `verify_native` checks
the candidate rather than rerunning or trusting the producer, and derives its
own SMT-LIB2 from the original IR and candidate. To bind already received
assembly, use `verify_native_assembly` with the expected IR, candidate,
request, and assembly bytes; it requires an exact match including the symbol
and helper body. No submitted SMT script or positive status is accepted by
these interfaces, and this is not an extension of the abstract
`RegisterAllocationEvidence` or an optimization-package proof.

## What is checked

- Values have a register home or a nonoverlapping, aligned, in-frame spill
  slot. The deterministic producer allocates across last-use intervals and
  spills under pressure. The checker independently derives live boundaries,
  rejects conflicting register homes, and checks the actual instruction stream
  delivers each operand from a valid symbolic state, including spills/reloads.
- The six SysV argument low words are read from EDI, ESI, EDX, ECX, R8D and R9D.
  Used arguments are captured in private frame slots before body instructions
  run, so a late or repeated `Param` is valid. The result is returned in EAX.
  Three scratch registers (RAX, R10, R11) are not allocated as value homes.
- Frame adjustments are checked at entry and exit. Stack memory is limited to
  disjoint incoming-argument, spill and save slots, with no writes to caller
  stack objects or the return address. Every callee-saved GPR written by the
  body (RBX, RBP, R12–R15) must have its own matched, full-width save and restore.
  The epilogue restores the entry stack pointer before `ret`.
- Novostone IR has no `Call` opcode. As an explicit testable lowering policy,
  selected `Copy` sites in `NativeRequest::identity_calls` use a direct call to
  one unit-local i32 identity helper. The checker verifies the outgoing EDI
  operand, 16-byte call-site RSP alignment, and the helper's actual instruction
  body: no stack manipulation or nested calls, no saved-register writes,
  and return of the original EDI low word in EAX. Other caller-saved GPRs and
  arithmetic flags are treated as clobbered. The generated helper actually
  overwrites the other caller-saved GPRs. Live-through register homes must be
  callee-saved; other live-through values use private spill slots. There is no
  support for external, indirect or caller-asserted callee contracts.
- A symbolic interpreter walks the candidate's machine instructions. For each
  SSA definition and return it compares the resulting i32 to independently
  derived IR semantics. A proved definition can be cut to a fresh, independent
  input for later spans; this is conservative for checks involving correlated
  values. Identical symbolic terms discharge by reflexivity; other equalities
  are encoded as exact 32-bit Boolean circuits in `QF_UF`. The script asks
  whether any modeled equality can fail. Only structural success and UNSAT
  via the existing SMT-LIB2/Satie conduit count as `Proved`.

A malformed layout, instruction stream or submitted byte string is
`Rejected`; a satisfiable negated obligation is also `Rejected` (some
counterexamples may be to the stronger cutpoint model). Unsupported input,
exhausted construction limits, and absent or inconclusive Satie never produce
a proof. `binding_checked` only establishes structural checking and derivation
of the obligation, not a positive discharge. Defaults bound the subject to
256 IR instructions, the candidate to 4,096 machine instructions and the
SMT-LIB2 to 512 KiB. Hard caps are respectively 4,096, 32,768 and 2 MiB, with
32 KiB maximum stack frame and symbolic depth 128. These bounds are not a
wall-clock deadline for Satie or a sandbox for untrusted requests.

The proof assumes a conforming x86-64 SysV caller and an available, writable
stack covering the frame and live call return addresses. It checks the stated
instruction/ABI model and exact GNU-compatible assembly rendering, not object
bytes, instruction encodings, relocations, linking, unwinding, OS/hardware
faults, asynchronous execution, or arbitrary binary correctness. The checker
and solver are trusted; no separately checkable SAT proof log is produced here.

## Execution test

`tests/pcc-native.cpp` checks scope, deterministic allocation, independent
translation checking, and rejection of tampered arithmetic, home assignments,
prologue/epilogue, frame and spill offsets, helper, call sites and assembly.
Passing the native C compiler path as its sole argument on x86-64 Linux LP64
also assembles, links and executes the generated code with a C11 driver. A
separate assembly wrapper checks RSP and all six full-width callee-saved GPRs;
the driver compares returns with the IR evaluator across wraparound and
comparison cases. No compiler invocation occurs in the library.
