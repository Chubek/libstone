# Chapter 2: Building and Installing

Configure with CMake, build the root project, and install headers, libraries, and tools. The qbfd target is header-only and the qld target is a static library.

The standalone `cmake -S yellowstone -B build-yellowstone` build also works.
Native ELF/PE/Mach-O linking does not need an external linker at runtime.
Optional external compatibility bridges have independent
`YELLOWSTONE_ENABLE_PE_BACKEND` / `YELLOWSTONE_ENABLE_MACHO_BACKEND` switches,
and `YELLOWSTONE_PE_LINKER` / `YELLOWSTONE_MACHO_LINKER` executable settings.
The bridges are OFF by default; their executables default to `lld-link` and
`ld64.lld`. Only explicit `--dynamic`/`--*-linker` requests use them, even when
built ON. ELF has the equivalent `YELLOWSTONE_ENABLE_ELF_BACKEND` switch.
See [backend build and usage](../ld/README.md).

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
