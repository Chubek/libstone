# Flintstone as a Libstone SDK

The SDK groups the existing libraries as CMake interface targets, not as a
single archive or a new ABI. `Flintstone::sdk` brings in the components
available in the configured build; individual targets allow smaller consumers.
The `flintstone` IL library and C ABI do not depend on the optional SDK
components. In particular, linking Moonstone or Novostone does not implicitly
authorize a safety proof, execute a specification, or convert Flintstone IL
into another component's IR.

The optimizer uses Blackstone's shipped SSA catalog. The default profile is
an ordered, bounded sequence of integer folds, simplifications, propagation,
and strength reduction followed by Flintstone dead-code elimination. Explicit
kernel lists retain the caller's order. Errors leave the input module intact.
Custom kernels are trusted code, not a sandbox. A valid SSA shape alone is
not proof of a custom rewrite's semantics.

## Build and consume

```sh
cmake -S flintstone -B build-flintstone \
  -DLIBSTONE_BUILD_YELLOWSTONE=ON -DMOONSTONE_BUILD_QMR=ON
cmake --build build-flintstone --parallel
cmake --install build-flintstone --prefix /path/to/sdk
```

```cmake
find_package(Flintstone CONFIG REQUIRED COMPONENTS sdk)
target_link_libraries(my_compiler PRIVATE Flintstone::sdk)
# Or request only: find_package(Flintstone CONFIG REQUIRED COMPONENTS novostone moonstone)
# and link Flintstone::novostone and Flintstone::moonstone directly.
```

| Flintstone component | Surface |
| --- | --- |
| `core`, `native`, `codegen` | IL and C ABI, Limestone/Sandstone native paths, Firestone code-generation plans |
| `firestone`, `blackstone` | Firestone libraries and the Blackstone rewrite runtime |
| `assembly` | Sandstone and its enabled assembler, ArchML, SymAS spec compiler, and SandSLAB |
| `tourchstone` | XBind and its spec compiler; native loader and invoker when built |
| `novostone` | Datalog, CCDB, DatabaseBasedIR, QueryBasedIR, SpeculativeIR, CostCC, and ActionSemantics |
| `moonstone` | Core and enabled Chisel, Syren, Supersonic, ExoML, Exotract, Plaid, Prolific, QMR, and Nerotypical Sequent IR |
| `wetstone`, `weighstone` | Program analyses and compiler integer arithmetic |
| `yellowstone`, `limestone` | Native object/linker SDK and full Limestone libraries, respectively, when built |

The standalone SDK leaves Yellowstone and the full Limestone backend opt-in.
Full Limestone requires compatible vendor revisions (see the repository
README). Exotract, ExoML discovery, and QMR also preserve their existing opt-in
switches. The SDK configuration requires Perl, Zlib, Boost headers (plus
Boost.JSON with the Sandstone spec compiler) and, for Prolific, LMDB. Set
`-DFLINTSTONE_BUILD_SDK=OFF` for the narrower standalone IL build. Components
requested by name but not built cause `find_package` to fail.

## Optimize IL

```sh
flintstone --list-kernels
flintstone -O --run affine --arg 10 flintstone/examples/affine.flint
flintstone --kernel ext_multiply_negative_one --kernel copy_propagation input.flint
```

In C++, call `flintstone::optimize(module, registry)` for the profile, or add
`blackstone_pipeline(directory, ordered_names, max_rounds)` to a pass manager.
The profile lowers High IL to Core when needed. It runs at most four Blackstone
rounds; reaching the cap does not guarantee a fixed point. A custom pass accepts
already-lowered built-in scalar SSA only; lower or scalarize unsupported
operations explicitly. From C, use `flintstone_optimize_default`.
The catalog is found beside the installed SDK or in the build's source tree;
`FLINTSTONE_KERNEL_DIR` or `--kernels DIR` can override its location.

## Validation

On Linux x86-64 with GCC 16.2.1, the standalone Release SDK with Moonstone
tests, QMR, and Exotract enabled passed 64 tests. A focused root Debug build
with full Limestone off and Yellowstone on passed 38 tests covering
Flintstone, Blackstone, and Novostone. The minimal Flintstone Debug build
without the SDK passed 8 tests. The SDK tests exercise a relocated consumer,
compare optimized and original execution across all `i8` inputs, and check
rollback on unsupported operations. Full Limestone could not build with the
checked-in Equinox-NG and EkippX APIs. Wetstone's standalone vendor test suite
was not part of the passing configurations.
