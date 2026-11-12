#include "memtkx/dsl/match.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Anchored regex subset (port of dsl::pattern<S>::matches)          */
/* ------------------------------------------------------------------ */

static bool mtkx__match_class(const char *cls, size_t cls_len, char c) {
  size_t i = 0;
  while (i < cls_len) {
    if (i + 2 < cls_len && cls[i + 1] == '-') {
      if (c >= cls[i] && c <= cls[i + 2]) {
        return true;
      }
      i += 3;
    } else {
      if (cls[i] == c) {
        return true;
      }
      ++i;
    }
  }
  return false;
}

static bool mtkx__match_one(char pat, const char *cls, size_t cls_len, char c) {
  if (pat == '[') {
    return mtkx__match_class(cls, cls_len, c);
  }
  if (pat == '.') {
    return true;
  }
  return pat == c;
}

static bool mtkx__match_here(const char *pat, size_t pat_len, const char *str,
                             size_t str_len) {
  const char *cls = NULL;
  size_t cls_len = 0;
  size_t advance = 1;
  char first;
  char quant = 0;
  const char *rest;
  size_t rest_len;

  if (pat_len == 0) {
    return str_len == 0;
  }
  if (pat[0] == '^') {
    return mtkx__match_here(pat + 1, pat_len - 1, str, str_len);
  }

  first = pat[0];
  if (first == '[') {
    const char *close = (const char *)memchr(pat + 1, ']', pat_len - 1);
    if (close == NULL) {
      return false;
    }
    cls = pat + 1;
    cls_len = (size_t)(close - cls);
    advance = (size_t)(close - pat) + 1;
  }

  if (advance < pat_len && (pat[advance] == '*' || pat[advance] == '+')) {
    quant = pat[advance++];
  }
  rest = pat + advance;
  rest_len = pat_len - advance;

  if (quant == '*') {
    /* zero occurrences first, then one or more */
    if (mtkx__match_here(rest, rest_len, str, str_len)) {
      return true;
    }
    while (str_len != 0 && mtkx__match_one(first, cls, cls_len, str[0])) {
      ++str;
      --str_len;
      if (mtkx__match_here(rest, rest_len, str, str_len)) {
        return true;
      }
    }
    return false;
  }
  if (quant == '+') {
    if (str_len == 0 || !mtkx__match_one(first, cls, cls_len, str[0])) {
      return false;
    }
    ++str;
    --str_len;
    while (str_len != 0 && mtkx__match_one(first, cls, cls_len, str[0])) {
      ++str;
      --str_len;
    }
    return mtkx__match_here(rest, rest_len, str, str_len);
  }

  if (str_len == 0 || !mtkx__match_one(first, cls, cls_len, str[0])) {
    return false;
  }
  return mtkx__match_here(rest, rest_len, str + 1, str_len - 1);
}

bool mtkx_pattern_matches_n(const char *pattern, size_t pattern_length, const char *input,
                            size_t input_length) {
  if (pattern == NULL) {
    pattern = "";
    pattern_length = 0;
  }
  if (input == NULL) {
    input = "";
    input_length = 0;
  }
  return mtkx__match_here(pattern, pattern_length, input, input_length);
}

bool mtkx_pattern_matches(const char *pattern, const char *input) {
  return mtkx_pattern_matches_n(pattern, pattern != NULL ? strlen(pattern) : 0, input,
                                input != NULL ? strlen(input) : 0);
}

/* ------------------------------------------------------------------ */
/*  Dispatch                                                          */
/* ------------------------------------------------------------------ */

static bool mtkx__run_otherwise(const mtkx_match_table *table, mtkx_match_key key,
                                void *args, void *result) {
  size_t i;
  for (i = 0; i < table->count; ++i) {
    const mtkx_match_clause *c = &table->clauses[i];
    if (c->kind == MTKX_MATCH_OTHERWISE) {
      if (c->handler != NULL) {
        c->handler(key, args, result, c->ctx);
      }
      return true;
    }
  }
  return false;
}

bool mtkx_match_int(const mtkx_match_table *table, intmax_t key, void *args, void *result) {
  mtkx_match_key k;
  size_t i;
  k.is_string = false;
  k.value = key;
  k.text = NULL;
  k.length = 0;
  for (i = 0; i < table->count; ++i) {
    const mtkx_match_clause *c = &table->clauses[i];
    if (c->kind == MTKX_MATCH_WHEN && c->value == key) {
      if (c->handler != NULL) {
        c->handler(k, args, result, c->ctx);
      }
      return true;
    }
  }
  return mtkx__run_otherwise(table, k, args, result);
}

bool mtkx_match_string(const mtkx_match_table *table, const char *key, size_t length,
                       void *args, void *result) {
  mtkx_match_key k;
  size_t i;
  k.is_string = true;
  k.value = 0;
  k.text = key != NULL ? key : "";
  k.length = key != NULL ? length : 0;
  for (i = 0; i < table->count; ++i) {
    const mtkx_match_clause *c = &table->clauses[i];
    if (c->kind == MTKX_MATCH_WHEN_PATTERN && c->pattern != NULL &&
        mtkx_pattern_matches_n(c->pattern, strlen(c->pattern), k.text, k.length)) {
      if (c->handler != NULL) {
        c->handler(k, args, result, c->ctx);
      }
      return true;
    }
  }
  return mtkx__run_otherwise(table, k, args, result);
}
