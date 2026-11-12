/* Parser combinators (guide 12.6) and semantic actions. */
#include <memtkx/memtkx.h>

#include "test_support.h"

static bool is_digit(char c, void *ctx) {
  (void)ctx;
  return c >= '0' && c <= '9';
}

static mtkx_parse_result any_char(mtkx_parse_input *in, void *ctx) {
  (void)ctx;
  if (mtkx_parse_input_eof(in)) {
    return mtkx_parse_fail_expected(in, "any character", MTKX_PARSE_SOFT);
  }
  return mtkx_parse_ok(mtkx_parse_value_char(mtkx_parse_input_consume(in)));
}

static void record_span(const mtkx_semantic_entry *semantics, size_t count,
                        const mtkx_parser_stage *stage, void *out, void *ctx) {
  (void)ctx;
  CHECK(count == 1 && strcmp(semantics[0].key, "number") == 0);
  *(mtkx_parser_stage *)out = *stage;
}

static void free_counter(void *ctx) { ++*(int *)ctx; }

static mtkx_parse_result user_value(mtkx_parse_input *in, void *ctx) {
  int *payload = (int *)mtkx_malloc(sizeof(int));
  (void)ctx;
  (void)in;
  *payload = 99;
  return mtkx_parse_ok(mtkx_parse_value_user(payload, mtkx_free));
}

int main(void) {
  /* sequences produce pairs; trailing input is a committed failure */
  {
    mtkx_parser *pair = mtkx_parser_seq(mtkx_parser_ch('a'), mtkx_parser_ch('b'));
    mtkx_parse_result good, trailing, bad;
    CHECK(pair != NULL);
    good = mtkx_run_parser(pair, "ab", 2);
    CHECK(good.ok && good.value->kind == MTKX_PV_PAIR);
    CHECK(good.value->as.pair.first->as.ch == 'a');
    CHECK(good.value->as.pair.second->as.ch == 'b');
    mtkx_parse_result_free(&good);

    trailing = mtkx_run_parser(pair, "abc", 3);
    CHECK(!trailing.ok && trailing.error.pos == 2);
    CHECK(trailing.error.kind == MTKX_PARSE_COMMITTED);
    CHECK(mtkx_parse_error_expects(&trailing.error, "<end-of-input>"));
    mtkx_parse_result_free(&trailing);

    bad = mtkx_run_parser(pair, "ac", 2);
    CHECK(!bad.ok && bad.error.pos == 1 && bad.error.kind == MTKX_PARSE_COMMITTED);
    CHECK(mtkx_parse_error_expects(&bad.error, "b"));
    mtkx_parse_result_free(&bad);

    /* committed failures block alternatives unless wrapped in try */
    {
      mtkx_parser *ac = mtkx_parser_seq(mtkx_parser_ch('a'), mtkx_parser_ch('c'));
      mtkx_parser *no_try = mtkx_parser_alt(mtkx_parser_retain(pair), mtkx_parser_retain(ac));
      mtkx_parser *with_try = mtkx_parser_alt(mtkx_parser_try(mtkx_parser_retain(pair)), ac);
      mtkx_parse_result r1 = mtkx_run_parser(no_try, "ac", 2);
      mtkx_parse_result r2 = mtkx_run_parser(with_try, "ac", 2);
      CHECK(!r1.ok);
      CHECK(r2.ok && r2.value->as.pair.second->as.ch == 'c');
      mtkx_parse_result_free(&r1);
      mtkx_parse_result_free(&r2);
      mtkx_parser_release(no_try);
      mtkx_parser_release(with_try);
    }
    mtkx_parser_release(pair);
  }

  /* soft failures merge expectations */
  {
    mtkx_parser *ab = mtkx_parser_alt(mtkx_parser_ch('a'), mtkx_parser_ch('b'));
    mtkx_parse_result r = mtkx_run_parser(ab, "z", 1);
    CHECK(!r.ok && r.error.pos == 0 && r.error.expected_count == 2);
    CHECK(mtkx_parse_error_expects(&r.error, "a") && mtkx_parse_error_expects(&r.error, "b"));
    mtkx_parse_result_free(&r);
    mtkx_parser_release(ab);
  }

  /* repetition, optional and labels */
  {
    mtkx_parser *digits = mtkx_parser_many(mtkx_parser_satisfy(is_digit, NULL, "digit"));
    mtkx_parser *maybe_sign = mtkx_parser_optional(mtkx_parser_ch('-'));
    mtkx_parser *number = mtkx_parser_labeled(
        mtkx_parser_seq(maybe_sign, mtkx_parser_retain(digits)), "number");
    mtkx_parse_result r;

    r = mtkx_run_parser(digits, "12345", 5);
    CHECK(r.ok && r.value->kind == MTKX_PV_LIST && r.value->as.list.count == 5);
    CHECK(r.value->as.list.items[4]->as.ch == '5');
    mtkx_parse_result_free(&r);

    r = mtkx_run_parser(digits, "", 0);
    CHECK(r.ok && r.value->as.list.count == 0);
    mtkx_parse_result_free(&r);

    r = mtkx_run_parser(number, "-42", 3);
    CHECK(r.ok && r.value->as.pair.first->kind == MTKX_PV_OPTIONAL);
    CHECK(r.value->as.pair.first->as.optional->as.ch == '-');
    mtkx_parse_result_free(&r);

    r = mtkx_run_parser(number, "42", 2);
    CHECK(r.ok && r.value->as.pair.first->as.optional == NULL);
    mtkx_parse_result_free(&r);

    r = mtkx_run_parser(number, "-x", 2);
    CHECK(!r.ok);
    mtkx_parse_result_free(&r);

    {
      mtkx_parser *labeled = mtkx_parser_labeled(mtkx_parser_ch('q'), "letter q");
      r = mtkx_run_parser(labeled, "x", 1);
      CHECK(!r.ok && r.error.expected_count == 1 &&
            strcmp(r.error.expected[0], "letter q") == 0);
      mtkx_parse_result_free(&r);
      mtkx_parser_release(labeled);
    }

    /* a repeated parser that consumes nothing terminates */
    {
      mtkx_parser *loop = mtkx_parser_many(mtkx_parser_optional(mtkx_parser_ch('x')));
      r = mtkx_run_parser(loop, "", 0);
      CHECK(r.ok && r.value->as.list.count == 1);
      mtkx_parse_result_free(&r);
      mtkx_parser_release(loop);
    }
    mtkx_parser_release(digits);
    mtkx_parser_release(number);
  }

  /* custom parsers, user values, ctx_free */
  {
    int freed = 0;
    mtkx_parser *any = mtkx_parser_new(any_char, &freed, free_counter);
    mtkx_parser *user = mtkx_parser_new(user_value, NULL, NULL);
    mtkx_parse_input in = mtkx_parse_input_make("xy", 2);
    mtkx_parse_result r = mtkx_parser_parse(any, &in);
    CHECK(r.ok && r.value->as.ch == 'x' && in.pos == 1);
    mtkx_parse_result_free(&r);
    mtkx_parser_retain(any);
    mtkx_parser_release(any);
    CHECK(freed == 0);
    mtkx_parser_release(any);
    CHECK(freed == 1);
    r = mtkx_run_parser(user, "", 0);
    CHECK(r.ok && r.value->kind == MTKX_PV_USER && *(int *)r.value->as.user.ptr == 99);
    mtkx_parse_result_free(&r);
    mtkx_parser_release(user);
    CHECK(mtkx_parser_seq(NULL, mtkx_parser_ch('a')) == NULL);
  }

  /* semantic actions */
  {
    mtkx_parser *digits = mtkx_parser_many(mtkx_parser_satisfy(is_digit, NULL, "digit"));
    mtkx_semantic_entry semantics[1];
    mtkx_parser_stage stage;
    mtkx_parse_input in = mtkx_parse_input_make("123abc", 6);
    mtkx_production productions[2];
    semantics[0].key = "number";
    semantics[0].value = "decimal";
    CHECK(mtkx_parse_with_action(digits, &in, semantics, 1, record_span, NULL, &stage));
    CHECK(stage.begin == 0 && stage.end == 3 && mtkx_strview_equal_cstr(stage.span, "123"));
    productions[0] = mtkx_production_make("number", record_span, NULL);
    productions[1] = mtkx_production_make("word", NULL, NULL);
    CHECK(mtkx_production_find(productions, 2, "word") == &productions[1]);
    CHECK(mtkx_production_find(productions, 2, "none") == NULL);
    mtkx_parser_release(digits);
  }

  /* file input */
  {
    char *text = NULL;
    char *message = NULL;
    size_t length = 0;
    CHECK(mtkx_load_parse_input_file("/definitely/not/here.txt", &text, &length, &message) ==
          MTKX_ERR_IO);
    CHECK(text == NULL && message != NULL && strstr(message, "cannot open") != NULL);
    mtkx_free(message);
    CHECK(mtkx_load_parse_input_file(__FILE__, &text, &length, &message) == MTKX_OK);
    CHECK(length > 100 && strstr(text, "file input") != NULL && strlen(text) == length);
    mtkx_free(text);
  }
  return 0;
}
