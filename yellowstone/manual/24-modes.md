# Chapter 24: Link Modes

Native ELF Exec creates static ET_EXEC; relocatable mode preserves supported
input relocations for a later link. Native PE creates fixed-base x86/x64 EXEs;
native Mach-O creates fixed-base x86-64 MH_EXECUTE with LC_UNIXTHREAD.
All three support static archives without external tools. Shared/PIE,
dynamic imports and native PE/Mach-O partial links are explicitly unsupported;
there is no automatic fallback.

The optional external bridges provide ELF ET_DYN/PIE, PE DLLs, Mach-O dylibs
and bundles. Enable the relevant CMake bridge and explicitly select
`--dynamic` or `--elf-linker`/`--pe-linker`/`--macho-linker`.

`lld-link` has no partial COFF (`-r`) mode. LLD's Mach-O backend does not
implement fully static Darwin images or partial links; qld rejects ignored
`-static`/`-r` requests by checking the generated image. This does not prevent
static archive extraction into executables/dylibs. See
[the backend guide](../ld/README.md) for precise toolchain requirements.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
