/* Result, Maybe, memoisation, lazy values, literal sets and expressions. */
#include <memtkx/memtkx.h>

#include "test_support.h"

MTKX_DEFINE_RESULT(int_result, int, const char *);
MTKX_DEFINE_RESULT(same_result, int, int); /* Result<T, T> works in C */
MTKX_DEFINE_RESULT(double_result, double, const char *);

static int twice(int v, void *ctx) {
  (void)ctx;
  return v * 2;
}
static double halve(int v, void *ctx) {
  (void)ctx;
  return v / 2.0;
}
static const char *shout(const char *e, void *ctx) {
  (void)e;
  return (const char *)ctx;
}
static int_result positive_only(int v, void *ctx) {
  (void)ctx;
  return v > 0 ? int_result_from_ok(v) : int_result_from_err("not positive");
}
static bool is_even(int v, void *ctx) {
  (void)ctx;
  return v % 2 == 0;
}
static mtkx_maybe_int half_if_even(int v, void *ctx) {
  (void)ctx;
  return v % 2 == 0 ? mtkx_maybe_int_some(v / 2) : mtkx_maybe_int_none();
}

static void square(const void *key, void *out, void *ctx) {
  ++*(int *)ctx;
  *(int *)out = *(const int *)key * *(const int *)key;
}

static void answer(void *out, void *ctx) {
  ++*(int *)ctx;
  *(int *)out = 42;
}

static long double km(long double v, void *ctx) {
  (void)ctx;
  return v * 1000.0L;
}
static long double meters(long double v, void *ctx) {
  (void)ctx;
  return v;
}
static long double cm(long double v, void *ctx) {
  (void)ctx;
  return v / 100.0L;
}

typedef struct scalar {
  double val;
} scalar;
static double scalar_value(const void *self) { return ((const scalar *)self)->val; }

static int panics = 0;
static void counting_panic(const char *message) {
  (void)message;
  ++panics;
}

int main(void) {
  /* --- Result --- */
  {
    int_result ok = int_result_from_ok(21);
    int_result err = int_result_from_err("boom");
    int out = 0;
    const char *e = NULL;
    double_result d;
    CHECK(int_result_is_ok(ok) && !int_result_is_err(ok));
    CHECK(int_result_unwrap(int_result_map(ok, twice, NULL)) == 42);
    CHECK(int_result_unwrap_or(err, -1) == -1);
    CHECK(int_result_get(ok, &out) && out == 21);
    CHECK(int_result_get_err(err, &e) && strcmp(e, "boom") == 0);
    CHECK(strcmp(int_result_unwrap_err(int_result_map_err(err, shout, (void *)"BOOM")),
                 "BOOM") == 0);
    CHECK(int_result_is_ok(int_result_map_err(ok, shout, (void *)"x")));
    CHECK(int_result_is_err(int_result_and_then(int_result_from_ok(-3), positive_only, NULL)));
    CHECK(int_result_unwrap(int_result_and_then(ok, positive_only, NULL)) == 21);
    d = MTKX_RESULT_MAP(double_result, ok, halve, NULL);
    CHECK(double_result_unwrap(d) == 10.5);
    d = MTKX_RESULT_MAP(double_result, err, halve, NULL);
    CHECK(double_result_is_err(d));
    CHECK(same_result_is_err(same_result_from_err(5)));
    CHECK(same_result_unwrap(same_result_from_ok(5)) == 5);
  }

  /* --- library result aliases --- */
  {
    mtkx_alloc_result failure = mtkx_alloc_result_from_err(MTKX_ALLOC_OUT_OF_MEMORY);
    CHECK(mtkx_alloc_result_unwrap_or(failure, 7) == 7);
    CHECK(mtkx_gc_result_unwrap(mtkx_gc_result_from_ok(3)) == 3);
  }

  /* --- panic handler on misuse --- */
  {
    /* a handler that returns is followed by abort(); only test install/restore */
    mtkx_panic_handler previous = mtkx_set_panic_handler(counting_panic);
    CHECK(mtkx_set_panic_handler(previous) == counting_panic);
    CHECK(panics == 0);
  }

  /* --- Maybe --- */
  {
    mtkx_maybe_int five = mtkx_maybe_int_some(5);
    mtkx_maybe_int none = mtkx_maybe_int_none();
    CHECK(mtkx_maybe_int_or_else(mtkx_maybe_int_map(five, twice, NULL), 0) == 10);
    CHECK(mtkx_maybe_int_or_else(mtkx_maybe_int_map(none, twice, NULL), -1) == -1);
    CHECK(!mtkx_maybe_int_has_value(mtkx_maybe_int_filter(five, is_even, NULL)));
    CHECK(mtkx_maybe_int_unwrap(mtkx_maybe_int_flat_map(mtkx_maybe_int_some(8), half_if_even,
                                                        NULL)) == 4);
    CHECK(!mtkx_maybe_int_flat_map(five, half_if_even, NULL).has_value);
    CHECK(MTKX_MAYBE_MAP(mtkx_maybe_double, five, halve, NULL).value == 2.5);
  }

  /* --- memoisation (guide 12.5) --- */
  {
    mtkx_memo memo;
    int computations = 0;
    int key = 7;
    int value = 0;
    int i;
    CHECK(mtkx_memo_init(&memo, sizeof(int), sizeof(int), square, &computations) == MTKX_OK);
    CHECK(mtkx_memo_call(&memo, &key, &value) == MTKX_OK && value == 49);
    CHECK(mtkx_memo_call(&memo, &key, &value) == MTKX_OK && value == 49 &&
          computations == 1);
    CHECK(mtkx_memo_contains(&memo, &key));
    mtkx_memo_clear_cache(&memo);
    CHECK(mtkx_memo_call(&memo, &key, &value) == MTKX_OK && value == 49 &&
          computations == 2);
    for (i = 0; i < 1000; ++i) {
      CHECK(mtkx_memo_call(&memo, &i, &value) == MTKX_OK && value == i * i);
    }
    CHECK(mtkx_memo_size(&memo) == 1000);
    for (i = 0; i < 1000; ++i) {
      CHECK(mtkx_memo_call(&memo, &i, &value) == MTKX_OK && value == i * i);
    }
    CHECK(computations == 2 + 999); /* key 7 was already cached */
    mtkx_memo_destroy(&memo);
    CHECK(mtkx_memo_init(&memo, 0, 4, square, NULL) == MTKX_ERR_INVALID_ARGUMENT);
  }

  /* --- lazy --- */
  {
    int calls = 0;
    int storage = 0;
    mtkx_lazy deferred;
    mtkx_lazy_init(&deferred, &storage, answer, &calls);
    CHECK(!mtkx_lazy_is_computed(&deferred));
    CHECK(*(int *)mtkx_lazy_get(&deferred) == 42 && mtkx_lazy_is_computed(&deferred));
    CHECK(*(int *)mtkx_lazy_force(&deferred) == 42 && calls == 1);
  }

  /* --- literal sets --- */
  {
    mtkx_literal_entry entries[3];
    mtkx_literal_set set;
    long double d = 0.0L;
    entries[0] = mtkx_lit("_km", km, NULL);
    entries[1] = mtkx_lit("_m", meters, NULL);
    entries[2] = mtkx_lit("_cm", cm, NULL);
    set = mtkx_literal_set_make(entries, 3);
    CHECK(mtkx_literal_set_parse(&set, "3.5_km", &d) == MTKX_OK && d == 3500.0L);
    CHECK(mtkx_literal_set_parse(&set, "12_m", &d) == MTKX_OK && d == 12.0L);
    CHECK(mtkx_literal_set_parse(&set, "250_cm", &d) == MTKX_OK && d == 2.5L);
    CHECK(mtkx_literal_set_parse(&set, "  7xyz_m", &d) == MTKX_OK && d == 7.0L);
    CHECK(mtkx_literal_set_parse(&set, "_m", &d) == MTKX_ERR_NOT_FOUND);
    CHECK(mtkx_literal_set_parse(&set, "5_mi", &d) == MTKX_ERR_NOT_FOUND);
    CHECK(mtkx_literal_set_parse(&set, "x_m", &d) == MTKX_ERR_PARSE);
    CHECK(mtkx_literal_set_parse(&set, "1e99999_m", &d) == MTKX_ERR_RANGE);
  }

  /* --- expression trees --- */
  {
    scalar sa = {3.0}, sb = {4.0};
    mtkx_expr a = mtkx_expr_terminal(scalar_value, &sa);
    mtkx_expr b = mtkx_expr_terminal(scalar_value, &sb);
    mtkx_expr bxa = mtkx_expr_mul(&b, &a);
    mtkx_expr sum = mtkx_expr_add(&a, &bxa);
    mtkx_expr two = mtkx_expr_const(2.0);
    mtkx_expr quotient = mtkx_expr_div(&sum, &two);
    mtkx_expr negated = mtkx_expr_neg(&quotient);
    mtkx_expr diff = mtkx_expr_sub(&negated, &two);
    double x[] = {1, 2, 3}, y[] = {4, 5, 6}, z[] = {7, 8, 9, 10}, out[3];
    mtkx_expr ex = mtkx_expr_array(x, 3), ey = mtkx_expr_array(y, 3),
              ez = mtkx_expr_array(z, 4);
    mtkx_expr yz = mtkx_expr_mul(&ey, &ez);
    mtkx_expr v = mtkx_expr_add(&ex, &yz);

    CHECK(mtkx_expr_eval(&sum) == 15.0);
    sa.val = 1.0; /* lazy: re-evaluation sees the new value */
    CHECK(mtkx_expr_eval(&sum) == 5.0);
    CHECK(mtkx_expr_eval(&diff) == -4.5);
    CHECK(mtkx_expr_length(&sum) == 1);
    CHECK(mtkx_expr_length(&v) == 3);
    CHECK(mtkx_expr_eval_into(&v, out, 3) == MTKX_OK);
    CHECK(out[0] == 29 && out[1] == 42 && out[2] == 57);
    CHECK(mtkx_expr_eval_into(&v, out, 4) == MTKX_ERR_INVALID_ARGUMENT);
  }
  return 0;
}
