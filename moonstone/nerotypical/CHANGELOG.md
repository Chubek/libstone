# Sequent IR extension — 2026-11-14

Upstream: `Chubek/libstone` `master` commit
`321bcc989ece9e08cc2ba318c21aa14d02d98a3c`.

## Added

- Header-only Sequent Core-inspired IR: types, terms, structured continuations,
  cut/command nodes, join/jump/recursive value groups and constructors.
- Type/scoping verifier, call-by-need reference evaluator, recursive thunks,
  deterministic statement rendering, semantic metrics and DOT visualization.
- Scoped `mu` reduction, beta reduction with sharing, constructor specialization,
  dead-let/join removal, conservative contification and verification gate.
- AST adapter for existing Nerotypical demo language; Satie type-result helpers.
- 15 regression test groups; standalone CMake target and end-to-end example.
- `AGENTS.md` and manuals documenting invariants and remaining acceptance gates.

## Preserved / not included in this distribution

The pinned, unchanged upstream `Analysis.hpp`, `Language.hpp`,
`SetTyping.hpp`, `StructuralTyping.hpp`, and `tests/set_typing.cpp` are not
copied into this overlay ZIP. Their exact expected Git blob SHAs are recorded
in `tools/restore_upstream.py`, which can complete the tree from a local clone
or pinned network fetch. The umbrella `Nerotypical.hpp` is the only original
file intentionally updated, to expose the new IR without modifying solver APIs.

## Verification performed

- GCC 14.2, C++20, `-Wall -Wextra -Wpedantic -Werror`
- Standalone header syntax checks for each core Sequent header
- CMake build, CTest executable with 15 test groups
- ASan/UBSan with leak detection enabled
- Executed `examples/joins.cpp`
- Optional AST adapter compiled against a shape-compatible temporary AST stub,
  **not** yet tested against actual Satie-linked `Language.hpp`

This release is a tested **experimental subset** and does not claim full
System Fω/Sequent Core or industrial GHC-equivalent optimizer completeness.
