# Build and embedding

Build standalone from the enclosing `moonstone` directory:

```sh
cmake -S moonstone -B build-moonstone -DCMAKE_BUILD_TYPE=Debug \
    -DMOONSTONE_BUILD_PLAID=ON -DMOONSTONE_BUILD_TESTS=ON
cmake --build build-moonstone -j4
ctest --test-dir build-moonstone --output-on-failure
```

Set `MOONSTONE_BUILD_CHISEL=OFF` etc. if only Plaid is required.
Headers are under `moonstone/plaid` and the umbrella is `Plaid.hpp`.
If included from a LibStone checkout, use `#include "moonstone/plaid/Plaid.hpp"`.

The plain C ABI is declared in `plaid/runtime/PlaidRuntime.h` and built by
`moonstone_plaid_runtime`. It supports a single-thread cooperative queue,
callbacks and bounded run steps. Callback pointers and user data remain owned
by the caller. Do not call destroy concurrently with run or from a callback.
This small ABI does not supply OS worker threads, selection or actor interfaces.

`Moonstone::Plaid` is an INTERFACE CMake target for headers and OS threads;
`Moonstone::PlaidRuntime` names the compiled C runtime implementation.

Examples are in `plaid/examples`; tests under `plaid/tests` illustrate error
paths, saturation, cancellation, channels, actors and frame validation.

The test build also compiles every public Plaid header as a separate C++20
translation unit with warnings treated as errors. The C ABI consumer is built
as strict C11. Lifecycle tests have CTest time limits; repeat fixed-seed stress:

```sh
ctest --test-dir build-moonstone -L stress --repeat until-fail:30 --output-on-failure
```

For sanitizer builds use a separate directory and instrument **both** C and C++:

```sh
cmake -S moonstone -B build-moonstone-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread'
cmake --build build-moonstone-tsan -j4
ctest --test-dir build-moonstone-tsan -R 'plaid_(loom|conduit|chorus|parallax)' --output-on-failure
```

Replace `thread` with `address,undefined` in another build for ASan/UBSan;
never combine TSan and ASan. Platform/compiler sanitizer runtime support is
required. Windows/BSD validation and root LibStone integration remain separate
acceptance gates.

`plaid-runtime-costs` measures 10,000 direct atomic operations, pool batch and
round-trip wakeups, channel transfers and empty selects. It checks results and
reports retained observer count; timings are smoke measurements, not throughput
guarantees. Use an optimized build and repeat on the deployment machine.
