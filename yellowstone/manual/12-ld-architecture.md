# Chapter 12: qld Architecture

qld separates loading, symbol discovery, layout, relocation, and emission. LinkState is the internal transaction state.

Format selection precedes native ELF loading. PE/COFF and Mach-O inputs,
including archive members, select the toolchain backends in `ld/PeBackend.cpp`
and `ld/MachOBackend.cpp`. They share response-file and process handling in
`ld/Backend.cpp`. See [the backend guide](../ld/README.md) for options,
dependencies, static/dynamic linking and staged-output behavior.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
