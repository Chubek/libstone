#ifndef TRACEML_INTEGER_OPS_H
#define TRACEML_INTEGER_OPS_H

#include "runtime.h"

/* Shared by the source interpreter and generated C runtime. On failure,
 * never perform the overflowing operation or write the output. */
static inline traceml_status traceml_integer_op(uint32_t op, int64_t a, int64_t b, int64_t *out) {
  switch (op) {
    case TRACEML_ADD:
      if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return TRACEML_OVERFLOW;
      *out = a + b;
      break;
    case TRACEML_SUB:
      if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return TRACEML_OVERFLOW;
      *out = a - b;
      break;
    case TRACEML_MUL:
      if ((a > 0 && (b > INT64_MAX / a || b < INT64_MIN / a)) ||
          (a < 0 && ((b > 0 && a < INT64_MIN / b) || (b < 0 && a < INT64_MAX / b)))) return TRACEML_OVERFLOW;
      *out = a * b;
      break;
    case TRACEML_EQ: *out = a == b; break;
    case TRACEML_LT: *out = a < b; break;
    case TRACEML_NEG:
      if (a == INT64_MIN) return TRACEML_OVERFLOW;
      *out = -a;
      break;
    default: return TRACEML_INVALID_PROGRAM;
  }
  return TRACEML_OK;
}

#endif
