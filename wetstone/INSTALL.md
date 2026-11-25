# Install / apply

Extract the `wetstone/` folder into the LibStone repository root. To run standalone checks:

```sh
cmake -S wetstone -B build-wetstone -DCMAKE_BUILD_TYPE=Release
cmake --build build-wetstone --parallel
ctest --test-dir build-wetstone --output-on-failure
```

The core uses the header-only `Wetstone::wetstone` target. Link
`Wetstone::alias_analysis` to use the compiled alias and dependence APIs.
Both targets are included in the installation's `WetstoneTargets.cmake`.
