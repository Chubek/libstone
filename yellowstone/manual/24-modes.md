# Chapter 24: Link Modes

For ELF, Exec creates ET_EXEC, shared and PIE create ET_DYN, and relocatable
mode preserves input relocations for a later link. PE maps Exec/Shared to
EXE/DLL and Mach-O maps them to MH_EXECUTE/MH_DYLIB; native `-bundle` is also
available. All three formats support linking static archives.

`lld-link` has no partial COFF (`-r`) mode. LLD's Mach-O backend does not
implement fully static Darwin images or partial links; qld rejects ignored
`-static`/`-r` requests by checking the generated image. This does not prevent
static archive extraction into executables/dylibs. See
[the backend guide](../ld/README.md) for precise toolchain requirements.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
