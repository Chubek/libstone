# Chapter 13: C ABI

ld/ld.h is the stable FFI boundary. Options are caller-owned for a call, diagnostics are malloc-owned, and qld_free helpers release returned memory.

The versioned `qld_elf_options_t`, `qld_pe_options_t` and
`qld_macho_options_t` APIs select formats without extending the
original `qld_options_t` layout. Their one-shot `_bytes` entry points support
in-memory output; incremental setters copy argument strings. A NULL `linker`
means native linking; a non-NULL executable explicitly requests an enabled
external compatibility bridge. `qld_*_linker()` queries the configured external
executable, not whether a native format is supported.
See [API details and the C11 example](../ld/README.md#apis-and-output-lifetime).

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
