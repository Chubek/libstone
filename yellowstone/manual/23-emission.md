# Chapter 23: ELF Emission

Executable, shared, and relocatable emitters write ELF fields explicitly with selected class and endianness, including section and symbol tables.

The native dynamic emitter is currently ELF64 little-endian x86-64. It emits
`PT_PHDR`, executable `PT_INTERP`, permission-separated `PT_LOAD`,
`PT_DYNAMIC`, `PT_GNU_STACK`, optional `PT_GNU_RELRO`, dynamic string/symbol
tables, SysV/GNU hashes, GOT/PLT and RELA sections. Shared objects and PIE use
ET_DYN; a link with a DSO input and no `--pie` uses dynamic ET_EXEC. These
images are executed by the Linux runtime loader in the regression suite.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
