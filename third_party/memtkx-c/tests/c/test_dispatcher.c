/* Port of tests/test_dispatcher.cpp: dispatch tables and predicates. */
#include <memtkx/memtkx.h>

#include "test_support.h"

typedef enum gc_state { UNMARKED = 0, MARKED = 1, FORWARDED = 2, PINNED = 3 } gc_state;

static void to_marked(mtkx_match_key k, void *args, void *out, void *ctx) {
  (void)k, (void)args, (void)ctx;
  *(gc_state *)out = MARKED;
}
static void to_forwarded(mtkx_match_key k, void *args, void *out, void *ctx) {
  (void)k, (void)args, (void)ctx;
  *(gc_state *)out = FORWARDED;
}
static void to_pinned(mtkx_match_key k, void *args, void *out, void *ctx) {
  (void)k, (void)args, (void)ctx;
  *(gc_state *)out = PINNED;
}

static gc_state trace_object(gc_state state, mtkx_address object) {
  static const mtkx_match_clause clauses[] = {
      {MTKX_MATCH_WHEN, UNMARKED, NULL, to_marked, NULL},
      {MTKX_MATCH_WHEN, MARKED, NULL, to_marked, NULL},
      {MTKX_MATCH_WHEN, FORWARDED, NULL, to_forwarded, NULL},
      {MTKX_MATCH_OTHERWISE, 0, NULL, to_pinned, NULL},
  };
  const mtkx_match_table table = mtkx_match_table_make(clauses, 4);
  gc_state next = UNMARKED;
  CHECK(mtkx_match_int(&table, state, &object, &next));
  return next;
}

static void label(mtkx_match_key k, void *args, void *out, void *ctx) {
  (void)k, (void)args;
  *(const char **)out = (const char *)ctx;
}

static bool positive(const void *v, void *ctx) {
  (void)ctx;
  return *(const int *)v > 0;
}
static bool even(const void *v, void *ctx) {
  ++*(int *)ctx;
  return *(const int *)v % 2 == 0;
}

static bool times_two(void *v, void *ctx) {
  (void)ctx;
  *(int *)v *= 2;
  return true;
}
static bool plus_one(void *v, void *ctx) {
  (void)ctx;
  *(int *)v += 1;
  return true;
}
static bool stop_if_big(void *v, void *ctx) {
  (void)ctx;
  return *(int *)v < 100;
}

int main(void) {
  CHECK(trace_object(UNMARKED, 0) == MARKED);
  CHECK(trace_object(FORWARDED, 0) == FORWARDED);
  CHECK(trace_object(PINNED, 0) == PINNED);

  /* string patterns and a table without fallback */
  {
    mtkx_match_clause clauses[3];
    mtkx_match_table table;
    const char *out = NULL;
    clauses[0] = mtkx_when_pattern("[0-9]+", label, (void *)"number");
    clauses[1] = mtkx_when_pattern("[a-z][a-z0-9_]*", label, (void *)"identifier");
    clauses[2] = mtkx_when(7, label, (void *)"seven");
    table = mtkx_match_table_make(clauses, 3);
    CHECK(mtkx_match_string(&table, "123", 3, NULL, &out) && strcmp(out, "number") == 0);
    CHECK(mtkx_match_string(&table, "x_1", 3, NULL, &out) && strcmp(out, "identifier") == 0);
    CHECK(!mtkx_match_string(&table, "1x", 2, NULL, &out));
    CHECK(mtkx_match_int(&table, 7, NULL, &out) && strcmp(out, "seven") == 0);
    CHECK(!mtkx_match_int(&table, 8, NULL, &out)); /* C++ threw here */
  }

  CHECK(mtkx_pattern_matches("a.c", "abc"));
  CHECK(mtkx_pattern_matches("^ab*c", "ac"));
  CHECK(mtkx_pattern_matches("ab+c", "abbbc"));
  CHECK(!mtkx_pattern_matches("ab+c", "ac"));
  CHECK(mtkx_pattern_matches("[a-c]*x", "abcabcx"));
  CHECK(!mtkx_pattern_matches("abc", "abcd"));
  CHECK(!mtkx_pattern_matches("[abc", "a"));
  CHECK(mtkx_pattern_matches("", ""));

  /* predicates: the same short-circuit sequence as the C++ test */
  {
    int even_checks = 0;
    int v;
    mtkx_predicate pos = mtkx_predicate_make(positive, NULL);
    mtkx_predicate ev = mtkx_predicate_make(even, &even_checks);
    mtkx_predicate both = mtkx_predicate_and(&pos, &ev);
    mtkx_predicate either = mtkx_predicate_or(&pos, &ev);
    mtkx_predicate non_positive = mtkx_predicate_not(&pos);
    v = -2;
    CHECK(!mtkx_predicate_test(&both, &v) && even_checks == 0);
    v = 3;
    CHECK(mtkx_predicate_test(&either, &v) && even_checks == 0);
    v = 4;
    CHECK(mtkx_predicate_test(&both, &v) && even_checks == 1);
    v = 3;
    CHECK(!mtkx_predicate_test(&both, &v) && even_checks == 2);
    v = -2;
    CHECK(mtkx_predicate_test(&either, &v) && even_checks == 3);
    v = -3;
    CHECK(!mtkx_predicate_test(&either, &v) && even_checks == 4);
    v = 0;
    CHECK(mtkx_predicate_test(&non_positive, &v));
    v = 1;
    CHECK(!mtkx_predicate_test(&non_positive, &v));
  }

  /* pipelines (guide 12.2) */
  {
    int value = 10;
    mtkx_pipe_stage stages[3];
    stages[0] = mtkx_pipe(times_two, NULL);
    stages[1] = mtkx_pipe(plus_one, NULL);
    CHECK(mtkx_pipe_run(&value, stages, 2) && value == 21);
    value = 10;
    CHECK(MTKX_PIPE(&value, mtkx_pipe(times_two, NULL), mtkx_pipe(plus_one, NULL)));
    CHECK(value == 21);
    value = 60;
    stages[2] = mtkx_pipe(stop_if_big, NULL);
    stages[0] = mtkx_pipe(times_two, NULL);
    stages[1] = mtkx_pipe(stop_if_big, NULL);
    stages[2] = mtkx_pipe(plus_one, NULL);
    CHECK(!mtkx_pipe_run(&value, stages, 3) && value == 120);
  }

  {
    unsigned features = MTKX_FEATURE_PIPELINE | MTKX_FEATURE_OPERATORS;
    CHECK(mtkx_has_feature(features, MTKX_FEATURE_PIPELINE));
    CHECK(!mtkx_has_feature(features, MTKX_FEATURE_AST));
    CHECK_STR_EQ(mtkx_feature_name(MTKX_FEATURE_REWRITE), "Rewrite");
  }
  return 0;
}
