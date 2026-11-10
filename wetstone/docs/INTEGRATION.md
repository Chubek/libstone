# LibStone integration

1. Overlay the ZIP's `wetstone/` directory into `libstone/wetstone/`.
2. Make `third_party/satie/include`, `third_party/equinox-ng`, and the relevant LibStone component sources available in the expected paths, or pass `WETSTONE_THIRD_PARTY_DIR` to CMake.
3. Build just Wetstone with `cmake -S wetstone -B build-wetstone` and run CTest.
4. If you want it built by the aggregate CMake build, add `add_subdirectory(wetstone)` in LibStone's root CMakeLists.txt, after any dependencies it needs. You can then link `Wetstone::wetstone`.

## Flintstone

`integration/Flintstone.hpp` converts a `flintstone::Function` into the analysis IR. It recognizes builtin `arith.*`, `cf.*`, `func.*`, and `mem.*` operations, respects Phi predecessor IDs and call symbol attributes, and rejects unsupported operand kinds instead of misinterpreting their representation.

`flintstone::arith` defaults to modular integers. Therefore `integration::convert(f)` defaults to conservatively abstracting its arithmetic results as unconstrained inputs. Pass `true` as the second argument only after proving your program's operation semantics match the mathematical-integer approximation or after replacing the transfer functions with bitvector-aware implementations.

## Firestone

`integration/Firestone.hpp` converts `firestone::ir::ControlFlowGraph` into the analysis IR. This is a structural bridge, not a proof of semantic equivalence for every Firestone opcode. Unrecognized operations with values are abstracted as unknown. The analyzer intentionally rejects unsupported literal/operand kinds.

## Analysis result lifecycle

- Validate input IR and check SSA dominance where it matters.
- Construct an analysis and always inspect `converged` / `truncated` before treating warnings as complete.
- Do not reuse cached results after IR changes unless the `AnalysisManager` revision advances or the relevant cache entries are invalidated.
- Do not rewrite or automatically apply repair suggestions without a separate proof/validation layer.
