# Plaid — concurrency, continuations and parallelism

Plaid is the fifth Moonstone subsystem. It supplies executable reference
continuations and coroutine machinery, schedulers and task scopes, CSP-style
channels, local actors, explicit parallel algorithms and rudimentary
concurrency analysis. See `../plaid/README.md` and `../plaid/manual/`.

This is not a complete Scheme continuation implementation, Go runtime,
Rust async lowering system, production race detector, or optimizing vectorizer.
The intentionally limited features are named in `../plaid/STATUS.yaml`, and
`../plaid/AGENTS.md` identifies exact implementations and required acceptance
tests for advancing each one.

Plaid should reuse the real LibStone Firestone CPS/SSA, concurrency, intrinsics
and compute foundations, and Wetstone alias/dataflow analyses, rather than
silently replacing them with look-alike abstractions. Its C and C++ APIs must
maintain exception, cancellation, memory ownership and shutdown invariants.
