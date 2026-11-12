# Prolific validation scope

Verified on Linux x86-64, GCC 16.2.1 / Clang 22.1.8, system LMDB 0.9.35 and the
checked-in Lua 5.5 / Kaguya / lmdbxx trees. No vendor sources were modified.

- Standalone Moonstone: build and **26/26 CTest checks** pass, including the
  preexisting twenty tests. Public Prolific headers compile individually with
  warnings as errors; the C embedding consumer compiles as C11.
  A Linux host exporting a conflicting Lua state-construction symbol confirms
  that replay uses its private vendored VM rather than the host's Lua ABI.
- Clang Debug with `-fsanitize=address,undefined` on both C and C++:
  **26/26 checks** pass, including Lua finalization and actual preloading.
- Separate Clang Debug with `-fsanitize=thread`: recorder, cache and
  multithreaded preload checks pass (**3/3 selected tests**). The preload host
  joins four pthread workers and the cache validates all sixty function events.
- Root CMake with Moonstone enabled and the other LibStone components disabled:
  build and **26/26 checks** pass. This is root-registration smoke coverage, not
  verification of the full LibStone dependency graph.
- Prolific disabled, tests/tools disabled, nonexistent vendor root: configuration
  and remaining library build pass; disabled profiling does not require vendors.
- Clang static analyzer on all six Prolific implementation files reports no
  diagnostics. `clang-format` was run on added native sources/headers.
- Installation to a separate prefix succeeds. The installed CLI passes the JSON,
  caching and Lua replay regression without `LD_LIBRARY_PATH`; an independently
  compiled C11 consumer links and executes against installed headers/libraries.

Main reproduction commands:

```sh
cmake -S moonstone -B build-moonstone -DMOONSTONE_BUILD_TESTS=ON
cmake --build build-moonstone --parallel
ctest --test-dir build-moonstone --output-on-failure

cmake -S moonstone -B /tmp/opencode/prolific-clang \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS=-fsanitize=address,undefined \
  -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -DMOONSTONE_BUILD_TESTS=ON
cmake --build /tmp/opencode/prolific-clang --parallel
ctest --test-dir /tmp/opencode/prolific-clang --output-on-failure
```

TSan uses a separate build with `-fsanitize=thread`; selection is
`ctest -R 'moonstone.prolific_(recorder|cache|injection)$'`. Static analysis uses
`clang++ --analyze -std=c++20` with Moonstone and the three vendor include paths.

Not verified: Windows/MSVC, macOS/BSD, non-x86 architectures, hostile-script
containment, crash recovery, disk-full commit fault injection, native symbol
resolution, production overhead or long-running workloads. Existing warnings in
unrelated Chisel/Supersonic/ExoML code were not repaired by this profiling change.
No production-readiness or semantic-safety certification follows from these tests.
