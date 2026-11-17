# Chapter 2: Building and Installing

Configure with CMake, build the root project, and install headers, libraries, and tools. The qbfd target is header-only and the qld target is a static library.

The standalone `cmake -S yellowstone -B build-yellowstone` build also works.
PE/COFF and Mach-O toolchain backends have independent
`YELLOWSTONE_ENABLE_PE_BACKEND` / `YELLOWSTONE_ENABLE_MACHO_BACKEND` switches,
and `YELLOWSTONE_PE_LINKER` / `YELLOWSTONE_MACHO_LINKER` executable settings.
They default to `lld-link` and `ld64.lld`. These executables are needed when
linking those formats, not when merely building or using qBFD.
See [backend build and usage](../ld/README.md).

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
