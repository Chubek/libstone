/**
 * @file memtkx/core/error.h
 * @brief Allocation and collection error vocabularies and their results.
 */
#ifndef MEMTKX_CORE_ERROR_H
#define MEMTKX_CORE_ERROR_H

#include "memtkx/core/base.h"
#include "memtkx/core/types.h"
#include "memtkx/dsl/result.h"

MTKX_BEGIN_DECLS

/** Reasons an allocation request can fail. */
typedef enum mtkx_alloc_error {
  MTKX_ALLOC_OUT_OF_MEMORY = 0,          /**< not enough capacity */
  MTKX_ALLOC_MISALIGNED = 1,             /**< alignment is not a power of two */
  MTKX_ALLOC_LARGE_OBJECT_EXCEEDED = 2,  /**< size outside a space's size policy */
  MTKX_ALLOC_ALLOCATION_FAILED = 3       /**< zero-byte request or bookkeeping failure */
} mtkx_alloc_error;

/** Reasons a collection or coordination request can fail. */
typedef enum mtkx_gc_error {
  MTKX_GC_MUTATOR_NOT_REGISTERED = 0,  /**< unknown thread id */
  MTKX_GC_SAFEPOINT_TIMEOUT = 1,       /**< a safepoint wait timed out */
  MTKX_GC_PLAN_UNAVAILABLE = 2,        /**< the plan could not complete (capacity/bookkeeping) */
  MTKX_GC_INVALID_TRACE = 3,           /**< a trace callback reported failure */
  MTKX_GC_COORDINATOR_BUSY = 4         /**< the coordinator cannot serve the request */
} mtkx_gc_error;

/** @return a static description such as "out of memory". */
MTKX_API const char *mtkx_alloc_error_string(mtkx_alloc_error error);

/** @return a static description such as "safepoint timed out". */
MTKX_API const char *mtkx_gc_error_string(mtkx_gc_error error);

/**
 * @brief Result of an allocation: an address or an mtkx_alloc_error.
 *        (C++: `AllocResult<Address>`.)
 */
MTKX_DEFINE_RESULT(mtkx_alloc_result, mtkx_address, mtkx_alloc_error);

/**
 * @brief Integer result of a coordination or phase operation.
 *        (C++: `GcResult<int>`.)
 */
MTKX_DEFINE_RESULT(mtkx_gc_result, int, mtkx_gc_error);

MTKX_END_DECLS

#endif /* MEMTKX_CORE_ERROR_H */
