# Novostone proof-carrying optimization

`Novostone::PCC` builds a deterministic optimization artifact from verified
Novostone i32 IR, records the source analysis and accepted rewrite prefix, emits
local SMT-LIB2 proof obligations, and models an abstract register allocation.
It does not claim that an artifact is proved merely because code was emitted.

## Pipeline

```text
verified ir::Function
  + RewriteConstraints
        |
        v
 pcc::build
        |
        +-- source SCCP fixed point and work counters
        +-- deterministic allowed QueryBasedIR rewrite prefix
        +-- source and transformed IR
        v
 pcc::emit_smt_proof
        |
        +-- replay every recorded rewrite and both endpoints
        +-- exact Boolean SMT-LIB2 obligations for supported rules
        v
 pcc::discharge_smt        (optional Satie conduit)
```

The artifact core uses schema version 1 and the explicit semantics identifier
`novostone.ir.i32.modulo-2^32.v1`. Addition, subtraction, and multiplication
are 32-bit modular operations. Equality and unsigned comparison produce i32
zero or one.

## Library API

```cpp
#include <novostone/PCC/SMTProof.hpp>

novostone::pcc::RewriteConstraints constraints;
constraints.allowed = {
    novostone::pcc::TransformKind::ConstantFolding,
    novostone::pcc::TransformKind::LocalCSE,
};
constraints.limits.max_rewrites = 100;

auto artifact = novostone::pcc::build(function, constraints);
auto proof = novostone::pcc::emit_smt_proof(artifact);
novostone::pcc::discharge_smt(proof);

if (!proof.all_proved()) {
  // Reject, retain as Unknown, or route through another checker.
}
```

`build` validates structural limits before running the ordinary IR verifier.
It imports the function into a private database, computes a complete source
SCCP fixed point, and accepts only the deterministic prefix ending before the
first disallowed rewrite. Resource exhaustion returns the last verified prefix
and records `StopCause::ResourceLimit`; incomplete SCCP is rejected.

`Artifact::canonical_core()` is deterministic and includes constraints, work
counters, analysis facts, rewrite records, selection status, and both IR
endpoints. Optional text and allocation attachments are deliberately
non-authoritative and excluded from that core.

### Proof coverage

The default rewrite allow-list contains only constant folding and local CSE.
The emitter has checked obligations for every built-in QueryBasedIR rewrite:

- i32 `add`, `sub`, `mul`, `eq`, and unsigned `ult` constant folding;
- local CSE, with both matching expression circuits encoded;
- add/sub-zero, multiply-zero/one, self-subtract/equality/comparison, and
  reducible `select` replacements;
- copy propagation and uniform phi elimination;
- constant or same-successor branch folding, including exact CFG and parallel
  phi repair;
- same-block load forwarding, including the earlier successful access,
  intervening stores, bounds checks, and trap order;
- rooted dead pure instruction and dead-cycle elimination, including
  mutually recursive phis; and
- SCCP constant replacement, with inductive checks of all constant transfers
  and omitted CFG edges.

Use `RewriteConstraints::all_query_rewrites()` to opt into the full optimizer.
The emitter reconstructs each adjacent state with the recorded options and
checks the rewrite, its provenance, endpoints, and structural side conditions.
A mismatch, an unsupported edit, or an exhausted proof-construction budget
rejects the proof artifact. It does not expose a partial proof.

Each script asserts the **negation** of one local correctness claim:

- `UNSAT` becomes `ObligationStatus::UnsatProved`;
- `SAT` becomes `ObligationStatus::SatCounterexample`; and
- an absent conduit, unsupported theory, solver failure, or exhausted external
  checker remains `ObligationStatus::Unknown`.

The exact i32 encoding uses pure Boolean SMT-LIB2 bit circuits under `QF_UF`.
The SMT result establishes the emitted model together with the structural
checks. Consumers must retain
the artifact, replay checks, semantics identifier, and proof-emitter trust
boundary; the SMT text alone is not an authenticated certificate of arbitrary
IR.

## Abstract register allocation

`RegisterAllocation.hpp` provides:

```cpp
auto evidence =
    novostone::pcc::allocate_registers(function, convention, constraints);
auto checked = novostone::pcc::validate_register_allocation(
    function, convention, caller_assignment, constraints);
```

The implementation derives SSA interference from Novostone liveness, checks
explicit fixed/allowed/forbidden registers and additional interference, assigns
register-only argument and return locations, and emits a pure-Boolean one-hot
SMT obligation. Deterministic coloring prefers caller-saved registers.

`EvidenceStatus::ObligationReady` means the abstract assignment is structurally
valid and its negated modeled conditions are expected to be unsatisfiable. It
does **not** mean a solver ran. Search exhaustion and certificate-size
exhaustion are `Unknown`; contradictory or impossible assignments are
`Rejected`.

This layer has no machine instructions, spill slots, stack arguments, call
clobbers, boundary moves, unwind state, or prologue/epilogue. Any use of a
callee-saved register therefore creates an unresolved preservation predicate
and keeps evidence `Unknown`. `native_calling_convention_proven` is always
false.

For checked native lowering, [`Native.hpp`](Native.md) implements a separate
x86-64 SysV Linux path for straight-line i32 functions. It verifies emitted
assembly against the IR, including stack spills, frame setup and teardown,
callee-saved registers, and clobbers at calls to a checked local helper.
It does not certify arbitrary callees or assembled object code.

## Diagnostic report and signed packages

```sh
novostone pcc input.nir \
  --frontend smtlib2 \
  --transforms constant_folding,local_cse \
  --max-rewrites 100 \
  --require-proof > input.pcc-report
```

The versioned, deterministic report contains the canonical artifact core,
source and optimized IR, submitted allow-list and limits, analysis facts and
work counters, complete transformation records, and length-prefixed SMT-LIB2
claims. `--frontend` is mandatory so no solver syntax is selected implicitly.

Without `--require-proof`, a report containing `Unknown` obligations is still
emitted successfully for later checking. With `--require-proof`, any non-UNSAT
obligation causes exit status 3 after emission. Invalid input, options,
unsupported rewrites, or malformed IR fail normally and do not produce a
success status.

Enable `NOVOSTONE_PCC_SIGNED_PACKAGES=ON` to build `Novostone::PCCPackage` and
the separate `pcc-pack`, `pcc-verify`, and `pcc-serve` commands (OpenSSL 3 Crypto).
The binary wire format signs the source and candidate IR plus constraints with
Ed25519. The verifier uses an independently configured public key and
regenerates analysis, transformations, and SMT proofs; it never accepts
packaged scripts or claimed results. The server can run as an OpenSSH forced
command with host and client authentication. The diagnostic report above is
not accepted as signed wire input. See [Package.md](Package.md) for the wire
format, command examples, and deployment limits.

## Satie integration

Satie is optional. `NOVOSTONE_PCC_USE_SATIE=ON` (the default) uses a compiled
`satie::satie` package when one is discoverable. If none is found, PCC still
builds and emits SMT-LIB2, but checking remains `Unknown`.
`NOVOSTONE_PCC_REQUIRE_SATIE=ON` makes absence a configuration error.
Combining `REQUIRE_SATIE=ON` with `USE_SATIE=OFF` is rejected as contradictory.

One reproducible standalone setup is:

```sh
cmake -S third_party/satie -B build-satie \
  -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DINSTALL_CLI=OFF \
  -DBUILD_D_FRONTENDS=OFF -DBUILD_STDPLUGIN=OFF
cmake --build build-satie --parallel
cmake --install build-satie --prefix /your/satie-prefix

cmake -S novostone -B build-novostone \
  -DCMAKE_PREFIX_PATH=/your/satie-prefix \
  -DNOVOSTONE_PCC_USE_SATIE=ON \
  -DNOVOSTONE_PCC_REQUIRE_SATIE=ON
```

A static installed `Novostone::PCC` target retains a link dependency on Satie,
so consumers must make both installation prefixes discoverable. A shared PCC
library absorbs its private static Satie implementation. Builds configured
without the conduit have no installed Satie dependency.

## Current boundaries

- Source-level checking covers the documented i32 IR, not arbitrary programs
  or interpreter step counts.
- The only implemented solver frontend choice is standardized SMT-LIB2.
- Satie is a proof conduit; unsupported input never becomes a positive result.
- Work limits are deterministic counters, not wall-clock or memory sandboxes.
- A diagnostic report is unsigned. Signed wire packages authenticate a
  configured key; they do not contain native code or native proofs.
- Native proofs cover the explicit x86-64 SysV assembly model, not object bytes.
