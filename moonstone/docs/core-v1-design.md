# Core v1 and fail-closed baseline design

This increment implements the first part of workstream A, not completion of the
four-module roadmap in `AGENTS.md`.

## Contracts and invariants

- Existing C++ aggregate types and include paths remain source compatible. Core
  services become a compiled library with a v1 C++ namespace and explicitly
  versioned C entry points. Static and shared packages export the same targets.
- `Result<T>` carries either a value or an error category, message and contextual
  source locations. A missing result is never interpreted as a proof.
- A source manager owns immutable, strictly validated UTF-8 snapshots. IDs are
  nonzero, manager-local, never recycled. Ranges are half-open byte offsets;
  positions are one-based **byte** columns and lines, with LF as line separator.
  EOF and empty ranges are valid; split UTF-8 scalars and reversed/out-of-range
  ranges fail. No source location is invented. Concurrent additions/lookups are
  synchronized; snapshots survive the manager.
- Source and symbol storage have explicit limits. Failed additions do not consume
  IDs or budget. Symbol identities use interned full strings, not unchecked hashes.
- Diagnostic retention is bounded by count, auxiliary entries and retained string bytes. Suppression
  and exhaustion are observable; suppressed errors still set `has_errors()`.
- C functions use fixed-width status codes, caller-owned outputs and opaque source
  handles. No C++ exception crosses the ABI. Source creation is bounded; failure
  clears its output. Handles must be valid and must not be destroyed concurrently.
- Checked signed arithmetic uses portable preconditions, never overflowing signed
  arithmetic. Malformed expression arity and unsupported grammar operations fail.
- CEGIS uses an explicit proof/counterexample/unknown/exhaustion result. The legacy
  optional-counterexample callback stays callable but cannot assert verification.
  Proof assumptions are retained; candidate-found is not a general proof.
- Repair ranking must be a strict total status/cost/ID ordering, without changing
  a candidate's verification status. Refutation, unknown, exhaustion and invalid
  edits must never become validated repairs.
- Chisel rejects unknown operations and reports an explicit analysis status.
  Diagnostics are collected from converged reachable states, identified by
  instruction position rather than fabricated/default source coordinates. This
  remains the small explicit-loan policy, not NLL or a memory-safety certificate.
- ExoML must reject unsupported operation names even for caller-constructed ASTs,
  before invoking any host callback. Parser validation alone is insufficient.
- Unsupported MemTKX/Satie/Equinox switches fail at configure time rather than
  implying an adapter is implemented. No vendor source is downloaded or copied.

## Build and failure semantics

Install headers, library, CMake package and pkg-config metadata; compile each
public header independently. Add opt-in ASan/UBSan or TSan (mutually exclusive),
compiler warnings, and GCC/Clang/MSVC CI. Root integration uses optional component
switches and preserves standalone operation. Root-build failures outside Moonstone
are recorded, not bypassed by weakening safety checks.

## Tests

Preserve the six baseline tests. Add positive, malformed, boundary and exhausted
cases for UTF-8, multiline spans, edits, result errors, bounded diagnostics and
symbols; concurrent source/symbol access; a C-compiled ABI consumer; malformed IR,
unknown operations, loops and unreachable Chisel blocks; signed arithmetic
boundaries and small exhaustive independent arithmetic checks; explicit CEGIS
proof, sampling/unknown, invalid counterexample and exhaustion cases. Build and
run an installed external consumer, shared/static variants and supported local
sanitizers. Vendor options must have configure-failure regression tests until
real linked adapters are delivered.

## Deferred (must remain documented)

Semantic IR/Flintstone/Wetstone adapters, projection/NLL analysis, real MemTKX,
typed/native Lua synthesis and solver/e-graph integration, expanded Syren semantic
witnesses, generated ExoML frontend, ABI probes and bindings, process isolation,
and production audit remain separate workstreams. No tests in this increment
certify those features.
