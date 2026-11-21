# FactIR

FactIR is enabled by default. Disable it with `--no-factir` or in `Cephyr.toml`:

```toml
[factir]
enabled = false
```

`--factir` overrides the TOML setting. `--no-cache` controls the separate
Archon generated-source cache; it does not disable FactIR.

FactIR runs per C translation unit after the QML and SDK rewrite passes, before
`--emit-mir` or the built-in Archon backend. It does not run during preprocessing,
syntax checking, assembly-only inputs, or when invoking a custom SDK backend.

## What is proved

The MIR text rewrite is a verified native MIR `i64` wrapping `add` with a known
zero operand to a `copy` of its other operand. Zero recognition is limited to
preceding same-block `const 0` instructions and chains of zero-preserving
copies and integer conversions of that constant. Satie checks the 64-bit
counterexample query using a bit-blasted modular adder. The pass requires
`UNSAT`, not `SAT` or `UNKNOWN`, and verifies the edited MIR. It does not
change checked arithmetic, reorder instructions, or drop any producer,
load, store, call, terminator, or other potentially observable operation.

After normal target legality checking, Archon can use committed `known_bits_i64`
rows to lower a conversion to a copy or select an immediate shift for a wrapping
multiply. It independently replays SSA bit transfers and uses only the bits
confirmed by both analyses, so stale or inconsistent claims cannot authorize
a rewrite. The conversion retains the low bits and sets the rest to zero or the
sign bit; elision requires proof that all those bits already match. Multiplying
an `i64` by an exact `2^k` produces the same low 64 bits as shifting left by
`k` for `0 <= k < 64`. Checked multiplication is not a candidate. Both
optimizations preserve producers and run on the RV64 native path and on the
AMD64/AArch64 scalar GAS paths. They do not change `--emit-mir`.

## Facts and consumers

After the rewrite, novostone/Datalog computes transitive SSA `dependency`
and `observable_input` relations. The analysis follows operand and block
argument flow within each function and follows values back from terminators
and conservatively modeled effectful or unknown operations.
The dump also includes the base relations with rows, such as `flow`, `sink`,
`control_edge`, `direct_call`, and `function` (function ordinal and name).
This is not memory alias analysis or an interprocedural dependency closure.
Separately, bounded forward propagation computes i64 zero/one masks for
constants, bitwise operations, conversions, safe known shifts, unsigned
narrow load results and integer comparisons. Block parameters intersect masks
across all predecessor edges, including backedges. Unknown parts are not
guessed, and the analysis neither folds memory reads nor infers their effects.
`known_bits_i64` dump tuples are `(function ordinal, SSA value, zero mask,
one mask)`, with masks displayed as unsigned decimal integers.

The LMDB environment is private, mode `0700`, and unique to one TU in one
invocation. It commits facts with the exact post-rewrite MIR, target identity,
MIR schema, and Datalog rule identity. It is removed on normal cleanup; there
is no disk reuse across TUs, targets, or invocations. This private disk I/O
also applies when compiling via the embedding API; disabling FactIR skips it.
The committed relations feed several backend decisions:

| Stage | Supported backend paths | Use |
| --- | --- | --- |
| Legalization and instruction selection | RV64I/RV64IM native, AMD64/AArch64 scalar GAS | Independently confirmed `known_bits_i64` facts enable the conversion and multiply optimizations above. |
| Machine optimization and scheduling | AMD64/AArch64 scalar GAS, RV64 native | Scalar emission skips a reload when the immediately preceding same-block instruction already left the exact input in the required scratch register. RV64 uses an equal-depth scheduling preference without changing verified dependencies or effects. |
| Register allocation and frame layout | RV64I/RV64IM LP64 native | Demand facts guide spill choices and nominate block-local spill slots for reuse; the backend independently verifies register and slot contents. ABI and cross-block values keep private homes. |
| Final emission | AMD64, AArch64, RV64I/RV64IM GAS | A confirmed control edge can fall through to the adjacent emitted block without losing edge copies. On AMD64 and AArch64 an immediately produced `i64` return can avoid an epilogue reload. Raw encodings and physical records do not use the fallthrough optimization. |

If Datalog
exhausts its fact or work budget, no facts are exported or consumed, and the
independently proved MIR rewrite can still apply.

## Explicit outputs

```sh
cephyr --emit-mir -o output.mir input.c \
  --factir-smtlib=obligation.smt2 --factir-facts=relations.txt
```

These optional side files require enabled FactIR and exactly one C input.
They are published after the primary compilation succeeds, to distinct new
files (not stdout), with mode `0600`. Each file is published atomically without
replacing an existing destination; the side files and the primary output are
not a single publication transaction. A unit with no candidate produces an SMT-LIB file
with no query; a unit with candidates produces one shared obligation and
records the solver result. Without these flags no FactIR files are published.
