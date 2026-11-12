#include "memtkx/dsl/literals.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "memtkx/core/units.h"

/* std::stold semantics: skip whitespace, parse a prefix, ignore the rest. */
static mtkx_status mtkx__parse_long_double(const char *text, size_t length,
                                           long double *out) {
  char buffer[128];
  char *heap = NULL;
  char *copy = buffer;
  char *end = NULL;
  long double value;
  mtkx_status status = MTKX_OK;

  if (length >= sizeof buffer) {
    heap = mtkx_strndup(text, length);
    if (heap == NULL) {
      return MTKX_ERR_NOMEM;
    }
    copy = heap;
  } else {
    memcpy(buffer, text, length);
    buffer[length] = '\0';
  }

  errno = 0;
  value = strtold(copy, &end);
  if (end == copy) {
    status = MTKX_ERR_PARSE;
  } else if (errno == ERANGE) {
    status = MTKX_ERR_RANGE;
  } else {
    *out = value;
  }
  mtkx_free(heap);
  return status;
}

mtkx_status mtkx_literal_set_parse_n(const mtkx_literal_set *set, const char *input,
                                     size_t length, long double *out) {
  size_t i;
  if (input == NULL || out == NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  for (i = 0; i < set->count; ++i) {
    const mtkx_literal_entry *e = &set->entries[i];
    size_t suffix_len = e->suffix != NULL ? strlen(e->suffix) : 0;
    if (length > suffix_len &&
        memcmp(input + (length - suffix_len), e->suffix, suffix_len) == 0) {
      long double number = 0.0L;
      mtkx_status status = mtkx__parse_long_double(input, length - suffix_len, &number);
      if (status != MTKX_OK) {
        return status;
      }
      *out = e->handler != NULL ? e->handler(number, e->ctx) : number;
      return MTKX_OK;
    }
  }
  return MTKX_ERR_NOT_FOUND;
}

mtkx_status mtkx_literal_set_parse(const mtkx_literal_set *set, const char *input,
                                   long double *out) {
  if (input == NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  return mtkx_literal_set_parse_n(set, input, strlen(input), out);
}

/* ------------------------------------------------------------------ */
/*  MemoryUnitsDSL                                                    */
/* ------------------------------------------------------------------ */

static long double mtkx__unit_scale(long double value, void *ctx) {
  const long double scale = *(const long double *)ctx;
  const long double scaled = value * scale;
  /* the C++ handlers convert to std::size_t: fractional bytes truncate.
   * Out-of-range values saturate instead of being undefined behaviour. */
  if (!(scaled > 0.0L)) {
    return 0.0L;
  }
  if (scaled >= (long double)SIZE_MAX) {
    return (long double)SIZE_MAX;
  }
  return (long double)(size_t)scaled;
}

static const long double mtkx__scale_b = 1.0L;
static const long double mtkx__scale_kb = 1024.0L;
static const long double mtkx__scale_mb = 1024.0L * 1024.0L;
static const long double mtkx__scale_gb = 1024.0L * 1024.0L * 1024.0L;

mtkx_status mtkx_units_parse_literal(const char *input, long double *out) {
  mtkx_literal_entry entries[4];
  mtkx_literal_set set;
  entries[0] = mtkx_lit("_B", mtkx__unit_scale, (void *)&mtkx__scale_b);
  entries[1] = mtkx_lit("_KB", mtkx__unit_scale, (void *)&mtkx__scale_kb);
  entries[2] = mtkx_lit("_MB", mtkx__unit_scale, (void *)&mtkx__scale_mb);
  entries[3] = mtkx_lit("_GB", mtkx__unit_scale, (void *)&mtkx__scale_gb);
  set = mtkx_literal_set_make(entries, 4);
  return mtkx_literal_set_parse(&set, input, out);
}
