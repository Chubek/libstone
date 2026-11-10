# Testing and verification

Build requirements: GCC or Clang with C++20 and `__int128`, CMake >=3.20. The seven standalone CTest programs cover core domains, CFG/dataflow, numerical-domain reductions, interprocedural summaries, symbolic formulas and paths, Andersen alias/ownership, and exhaustive finite-range checks for arithmetic domains and known bits.

```sh
cmake -S wetstone -B build-wetstone -DWETSTONE_USE_FLINTSTONE=OFF
cmake --build build-wetstone --parallel
ctest --test-dir build-wetstone --output-on-failure
```

No external dependency is required to run these core tests. When Satie is absent, SAT result tests accept `Unknown` as specified; this is **not** evidence that Satie itself passed. Similarly, Equinox-NG is only compiled in after the upstream include files become available.

Recommended further validation before production deployment: sanitizer builds (`-fsanitize=address,undefined`), adversarial CFG/SSA fuzzing, cross-testing interval and congruence operators against a concrete interpreter, full vendor-version adapter compilation, field-sensitive heap tests, and differential testing with mature static analyzers.

**Known limits:** the interval engine is neither a modular-arithmetic verifier nor a polyhedra implementation; the shape-analysis and fixpoint components are intentionally bounded; the global pointer analysis is flow-insensitive; the repair engine produces suggestions, not authenticated patches.
