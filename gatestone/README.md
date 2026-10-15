# Gatestone

`gateml/` is a C++20 implementation of a new, strict ML-style language built
on the vendored MetaChain toolchain. See [its manual](gateml/README.md).

```sh
cmake -S gatestone -B build-gatestone
cmake --build build-gatestone --parallel
ctest --test-dir build-gatestone --output-on-failure
build-gatestone/gateml/gateml-cli run gatestone/gateml/examples/lists.gml
```

The `gatebindle/` directories remain separate, unimplemented scaffolding;
they are not built. The former frontend-definition DSL in
`.decomissioned/gatestone` is not the syntax of this language and is not modified.
