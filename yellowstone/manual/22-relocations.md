# Chapter 22: Relocations

Relocation application computes S, A, and P from canonical symbols and section addresses, checks bounds, and reports truncation.

Native ELF64 x86-64 dynamic links additionally classify each allocated
relocation before emission. PIC GOTPCREL/GOTPCRELX forms target generated GOT
slots, PLT32 targets generated lazy/eager PLT slots, and writable absolute
references become `R_X86_64_RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, or symbol-based
`R_X86_64_64` runtime RELA records. Address-dependent 32-bit/text relocations
that cannot remain valid in a shared image are rejected with a PIC diagnostic.
TLS, IFUNC, copy and unsupported instruction encodings remain explicit errors.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
