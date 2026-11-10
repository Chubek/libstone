# Wetstone implementation build report

- Date: 2026-11-10.
- Source reference: `https://github.com/Chubek/libstone`, default branch `master`. The repository's `wetstone` headers were empty in the examined revision.
- Delivery: a `wetstone/` overlay; the complete parent repository is **not** bundled.
- C++20, header-only interface library; local build uses GNU C++ compiler.
- `cmake -S wetstone -B build-wetstone -DWETSTONE_USE_FLINTSTONE=OFF`: successful.
- `cmake --build build-wetstone -j4`: successful.
- `ctest --test-dir build-wetstone --output-on-failure`: **8 of 8 suites passed** (`core`, `cfg`, `domains`, `interprocedural`, `symbolic`, `pointer`, `soundness`, `typed`).
- Test executables are compiled with `-UNDEBUG` so the assert-based suite is live even in Release builds (where `-DNDEBUG` would otherwise disable every check).
- Re-validated under AddressSanitizer + UndefinedBehaviorSanitizer (Debug): all 8 suites pass clean, including the exhaustive small-range bitvector/typed/octagon/narrowing soundness loops.
- Examples `interval.cpp`, `pointer.cpp`, `smt.cpp` compiled and ran individually using `g++ -std=c++20` and the new headers.
- Soundness suite exhaustively samples selected small integer ranges; this is **not** a general formal proof.
- The local environment could not perform a network Git clone; the original repository layout and LibStone interfaces were examined through GitHub access.
- `third_party/` was not tracked on the remote LibStone `master` revision examined. No upstream libraries have been bundled or copied. Satie and Equinox-NG headers and interfaces were inspected in their separate public repositories; integrations are conditional and **not integration-tested** against the intended vendored snapshots.
- Full LibStone/Flintstone build, Satie-powered SAT/SMT checks, and Equinox-NG saturation were **not** validated in this checkout.
- Abstract numerical transfer operations use specific mathematical-integer semantics and conservative fallbacks; consult README and `docs/DOMAINS.md` before treating results as compiler-correctness proofs.
