# Chapter 12: qld Architecture

qld separates loading, symbol discovery, layout, relocation, and emission. LinkState is the internal transaction state.

Format selection precedes native loading. ELF, PE/COFF and Mach-O share
loading, symbol resolution and lazy archive extraction. `ld/NativeOptions.cpp`
validates native arguments; `ld/NativeImage.cpp` lays out, relocates and emits
bounded PE/Mach-O static images. ELF retains its native layout/relocation/emission
pipeline and script SDK. Selecting a format, `--static`, or `-l` never implies
permission to spawn a linker.

Explicitly requested compatibility bridges in `ld/ElfBackend.cpp`,
`ld/PeBackend.cpp` and `ld/MachOBackend.cpp` share response-file/process handling
in `ld/Backend.cpp`. They are disabled by default. See [the backend guide](../ld/README.md) for options,
dependencies, static/dynamic linking and staged-output behavior.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
