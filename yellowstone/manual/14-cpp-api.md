# Chapter 14: C++ API

PE and Mach-O use `qld::Options::pe` / `macho` and
`qld::Linker::setPeOptions` / `setMachOOptions`, mirroring the existing ELF
configuration. See [the backend guide](../ld/README.md) and
`ld/tests/formats_api.cpp` for exercised one-shot and incremental examples.

ld/ld.hpp provides owning Options, Expected results, RAII Linker builders, and script validation without exposing internal state.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
