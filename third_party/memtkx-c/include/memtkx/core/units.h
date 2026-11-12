/**
 * @file memtkx/core/units.h
 * @brief Binary memory quantities (B, KB = 1024 B, MB, GB) and conversions.
 *
 * The C++ user-defined literals `64_KB` become macros: `MTKX_KB(64)`.
 * Despite the short names, scaling is binary (powers of 1024).
 */
#ifndef MEMTKX_CORE_UNITS_H
#define MEMTKX_CORE_UNITS_H

#include <stddef.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/** @p n bytes as size_t. */
#define MTKX_B(n) ((size_t)(n))
/** @p n * 1024 bytes as size_t. */
#define MTKX_KB(n) ((size_t)((unsigned long long)(n) * 1024ULL))
/** @p n * 1024^2 bytes as size_t. */
#define MTKX_MB(n) ((size_t)((unsigned long long)(n) * 1024ULL * 1024ULL))
/** @p n * 1024^3 bytes as size_t. */
#define MTKX_GB(n)                                                            \
  ((size_t)((unsigned long long)(n) * 1024ULL * 1024ULL * 1024ULL))

/** Whole KiB in @p bytes (integer division). */
MTKX_INLINE size_t mtkx_bytes_to_kib(size_t bytes) { return bytes / 1024u; }
/** @p kib KiB in bytes. */
MTKX_INLINE size_t mtkx_kib_to_bytes(size_t kib) { return kib * 1024u; }
/** Whole MiB in @p bytes (integer division). */
MTKX_INLINE size_t mtkx_bytes_to_mib(size_t bytes) { return bytes / (1024u * 1024u); }
/** @p mib MiB in bytes. */
MTKX_INLINE size_t mtkx_mib_to_bytes(size_t mib) { return mib * 1024u * 1024u; }

/**
 * @brief Parses a runtime quantity such as "1.5_MB" or "64_KB".
 *
 * Recognised suffixes, tried in this order: `_B`, `_KB`, `_MB`, `_GB`.
 * The numeric prefix is parsed like strtold (leading whitespace allowed;
 * trailing characters after the number are ignored). The scaled value is
 * truncated to a whole number of bytes, as in the C++ `MemoryUnitsDSL`.
 *
 * @param input NUL-terminated literal text.
 * @param out   receives the byte count as long double.
 * @return MTKX_OK, MTKX_ERR_NOT_FOUND (no suffix matched),
 *         MTKX_ERR_PARSE (no number) or MTKX_ERR_RANGE (overflow).
 */
MTKX_API mtkx_status mtkx_units_parse_literal(const char *input, long double *out);

MTKX_END_DECLS

#endif /* MEMTKX_CORE_UNITS_H */
