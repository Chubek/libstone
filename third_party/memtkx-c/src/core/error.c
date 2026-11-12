#include "memtkx/core/error.h"

const char *mtkx_alloc_error_string(mtkx_alloc_error error) {
  switch (error) {
    case MTKX_ALLOC_OUT_OF_MEMORY:
      return "out of memory";
    case MTKX_ALLOC_MISALIGNED:
      return "invalid alignment";
    case MTKX_ALLOC_LARGE_OBJECT_EXCEEDED:
      return "large object size exceeded";
    case MTKX_ALLOC_ALLOCATION_FAILED:
      return "allocation failed";
  }
  return "unknown allocation error";
}

const char *mtkx_gc_error_string(mtkx_gc_error error) {
  switch (error) {
    case MTKX_GC_MUTATOR_NOT_REGISTERED:
      return "mutator is not registered";
    case MTKX_GC_SAFEPOINT_TIMEOUT:
      return "safepoint timed out";
    case MTKX_GC_PLAN_UNAVAILABLE:
      return "GC plan is unavailable";
    case MTKX_GC_INVALID_TRACE:
      return "invalid object trace";
    case MTKX_GC_COORDINATOR_BUSY:
      return "collector coordinator is busy";
  }
  return "unknown GC error";
}
