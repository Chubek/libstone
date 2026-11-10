# Install / apply

Extract the `wetstone/` folder into the LibStone repository root. To run standalone checks:

```sh
cmake -S wetstone -B build-wetstone -DCMAKE_BUILD_TYPE=Release
cmake --build build-wetstone --parallel
ctest --test-dir build-wetstone --output-on-failure
```

The project is header-only and `Wetstone::wetstone` is an INTERFACE CMake target. To integrate with LibStone's root CMake build, add `add_subdirectory(wetstone)` to `libstone/CMakeLists.txt` and link it from relevant consumers. The repository's `third_party/` directory is a local dependency root, not included in this ZIP.
