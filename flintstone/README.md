# Flintstone

Flintstone is a C++20 multi-level, parametric-dialect IL framework with a C ABI
and a textual IL tool. It reuses Firestone SSA/CFG/VMs, Blackstone rewrite kernels,
Limestone scheduling and register allocation, Sandstone SymAS encoding, and
Tourchstone XBind bindings. Native instruction selection and ABI lowering are
explicit target adapters, not guessed from an ISA name.

Dialects: `arith`, `cf` (with `trap`), `func`, `mem`, `math`, `vector`,
`tuple`, `ptr`, `atomic`, `vm`. Targets: portable `vm`/`vm-stack` bytecode,
a generic Sandstone native bridge, shipped `x86-64`/`aarch64`/`riscv64` tiny
native targets with verified machine encodings, all four RegTL allocators, and
all nine Tourchstone binding targets (plus a 27-architecture Sandstone
registry).

A standard library lives in `stdlib/` and is imported with `import "std";`;
see the [manual](manual/10-stdlib-and-imports.md).

```sh
cmake -S flintstone -B /tmp/opencode/flintstone-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/opencode/flintstone-build --parallel 2
ctest --test-dir /tmp/opencode/flintstone-build --output-on-failure
/tmp/opencode/flintstone-build/flintstone --run affine --arg 10 flintstone/examples/affine.flint
# 42
```

See the [manual](manual/README.md) for the language, dialect/target extension APIs,
embedding, backend contracts, build options, examples and supported boundaries.
