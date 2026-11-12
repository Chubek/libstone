/**
 * @file memtkx/core/version.h
 * @brief MemTKX version numbers (single source of truth; CMake reads them).
 */
#ifndef MEMTKX_CORE_VERSION_H
#define MEMTKX_CORE_VERSION_H

#define MTKX_VERSION_MAJOR 2
#define MTKX_VERSION_MINOR 0
#define MTKX_VERSION_PATCH 0

/** Encoded as MAJOR * 10000 + MINOR * 100 + PATCH. */
#define MTKX_VERSION                                                          \
  (MTKX_VERSION_MAJOR * 10000 + MTKX_VERSION_MINOR * 100 + MTKX_VERSION_PATCH)

#define MTKX_VERSION_STRING "2.0.0"

#endif /* MEMTKX_CORE_VERSION_H */
