/* PEG definitions, channels, the matcher and PEG combinators. */
#include <memtkx/memtkx.h>

#include "test_support.h"

typedef struct tokens {
  char text[16][16];
  size_t line[16];
  size_t column[16];
  size_t count;
} tokens;

static void collect(mtkx_peg_match *m, void *ctx) {
  tokens *t = (tokens *)ctx;
  size_t n = m->value.length < 15 ? m->value.length : 15;
  CHECK(t->count < 16);
  memcpy(t->text[t->count], m->value.data, n);
  t->text[t->count][n] = '\0';
  t->line[t->count] = m->line;
  t->column[t->count] = m->column;
  ++t->count;
}

static void count(mtkx_peg_match *m, void *ctx) {
  (void)m;
  ++*(int *)ctx;
}

int main(void) {
  CHECK(mtkx_peg_channel_validate("@COMMENTS") == MTKX_OK);
  CHECK(mtkx_peg_channel_validate("COMMENTS") == MTKX_ERR_INVALID_ARGUMENT);
  CHECK(mtkx_peg_channel_validate("") == MTKX_ERR_INVALID_ARGUMENT);
  CHECK(mtkx_peg_pattern_error("[a-z") != NULL);
  CHECK(mtkx_peg_pattern_error("[a-z]+") == NULL);

  /* --- tokenizing parse with an ignore channel --- */
  {
    tokens idents;
    int numbers = 0;
    mtkx_peg_definition *peg = mtkx_peg_definition_create();
    mtkx_peg_rule *ident = NULL, *number = NULL, *space = NULL;
    mtkx_peg_parse_result r;
    memset(&idents, 0, sizeof idents);
    CHECK(peg != NULL);
    CHECK(mtkx_peg_definition_add_rule(peg, "[a-z][a-z0-9]*", collect, &idents, &ident) ==
          MTKX_OK);
    CHECK(mtkx_peg_definition_add_rule(peg, "[0-9]+", count, &numbers, &number) == MTKX_OK);
    CHECK(mtkx_peg_definition_add_rule(peg, "[ \n]+", NULL, NULL, &space) == MTKX_OK);
    CHECK(mtkx_peg_rule_set_channel(space, MTKX_PEG_IGNORE_CHANNEL) == MTKX_OK);
    CHECK(mtkx_peg_rule_set_channel(space, "nope") == MTKX_ERR_INVALID_ARGUMENT);
    CHECK(mtkx_peg_definition_add_rule(peg, "[x", NULL, NULL, NULL) ==
          MTKX_ERR_INVALID_ARGUMENT);
    CHECK(ident->order == 0 && number->order == 1 && space->order == 2);

    r = mtkx_peg_definition_parse(peg, "ab 12 c3\nd", 10);
    CHECK(r.success && r.offset == 10 && r.message[0] == '\0');
    CHECK(idents.count == 3 && numbers == 1);
    CHECK_STR_EQ(idents.text[0], "ab");
    CHECK_STR_EQ(idents.text[1], "c3");
    CHECK_STR_EQ(idents.text[2], "d");
    CHECK(idents.line[2] == 2 && idents.column[2] == 1);

    r = mtkx_peg_definition_parse(peg, "ab\n  #", 6);
    CHECK(!r.success && r.offset == 5 && r.line == 2 && r.column == 3);
    CHECK(strstr(r.message, "no PEG rule matched at offset 5") != NULL);

    /* --- derived definitions see parent rules first --- */
    {
      int hashes = 0;
      mtkx_peg_definition *child = mtkx_peg_definition_derive(peg);
      const mtkx_peg_rule *visible[8];
      CHECK(mtkx_peg_definition_add_rule(child, "#", count, &hashes, NULL) == MTKX_OK);
      CHECK(mtkx_peg_definition_rules(child, visible, 8) == 4);
      CHECK(visible[0] == ident && visible[3]->order == 0);
      CHECK(mtkx_peg_definition_parent(child) == peg);
      r = mtkx_peg_definition_parse(child, "ab #", 4);
      CHECK(r.success && hashes == 1);
      CHECK(mtkx_peg_definition_rules(peg, NULL, 0) == 3); /* parent unchanged */
      mtkx_peg_definition_destroy(child);
    }

    /* --- the matcher --- */
    {
      tokens seen;
      int wild = 0;
      mtkx_peg_matcher *m = mtkx_peg_matcher_create(peg, "ab 12 ?x", 8);
      memset(&seen, 0, sizeof seen);
      CHECK(m != NULL);
      while (!mtkx_peg_matcher_is_spent(m)) {
        CHECK(mtkx_peg_matcher_feed(m, mtkx_peg_rule_arm(ident, collect, &seen)) == MTKX_OK);
        CHECK(mtkx_peg_matcher_feed(m, mtkx_peg_rule_arm(number, collect, &seen)) == MTKX_OK);
        CHECK(mtkx_peg_matcher_feed(m, mtkx_peg_wildcard_arm(count, &wild)) == MTKX_OK);
      }
      CHECK(seen.count == 3 && wild == 1);
      CHECK_STR_EQ(seen.text[0], "ab");
      CHECK_STR_EQ(seen.text[1], "12");
      CHECK_STR_EQ(seen.text[2], "x");
      CHECK(mtkx_peg_matcher_position(m) == 8);
      mtkx_peg_matcher_destroy(m);
    }

    /* --- channel filters --- */
    {
      tokens seen;
      int wild = 0;
      mtkx_peg_matcher *m = mtkx_peg_matcher_create(peg, "ab", 2);
      memset(&seen, 0, sizeof seen);
      CHECK(mtkx_peg_rule_set_channel(number, "@NUM") == MTKX_OK);
      CHECK(mtkx_peg_matcher_only(m, "@NUM") == MTKX_OK);
      CHECK(!mtkx_peg_matcher_channel_allowed(m, MTKX_PEG_DEFAULT_CHANNEL));
      CHECK(mtkx_peg_matcher_channel_allowed(m, "@NUM"));
      CHECK(mtkx_peg_matcher_feed(m, mtkx_peg_rule_arm(ident, collect, &seen)) == MTKX_OK);
      CHECK(seen.count == 0 && mtkx_peg_matcher_position(m) == 0); /* filtered, kept */
      CHECK(mtkx_peg_matcher_feed(m, mtkx_peg_wildcard_arm(count, &wild)) == MTKX_OK);
      CHECK(wild == 1 && mtkx_peg_matcher_position(m) == 1);
      CHECK(mtkx_peg_matcher_exclude(m, "@NUM") == MTKX_OK);
      CHECK(mtkx_peg_matcher_channel_allowed(m, MTKX_PEG_DEFAULT_CHANNEL));
      CHECK(!mtkx_peg_matcher_channel_allowed(m, "@NUM"));
      CHECK(mtkx_peg_matcher_include(m, "@X") == MTKX_OK);
      CHECK(mtkx_peg_matcher_channel_allowed(m, "@X"));
      CHECK(mtkx_peg_matcher_channel_allowed(m, "@NUM")); /* C++: include keeps the list */
      mtkx_peg_matcher_destroy(m);

      m = mtkx_peg_matcher_create(peg, "42 a", 4);
      CHECK(mtkx_peg_matcher_feed(m, mtkx_peg_channel_arm("@NUM", collect, &seen)) == MTKX_OK);
      CHECK(seen.count == 1 && strcmp(seen.text[0], "42") == 0);
      mtkx_peg_matcher_destroy(m);
    }

    /* --- must_fail, fire_on_neg and if_succeed --- */
    {
      int negs = 0, follow = 0;
      mtkx_peg_definition *d = mtkx_peg_definition_create();
      mtkx_peg_rule *not_a = NULL, *a = NULL, *a_follow = NULL;
      CHECK(mtkx_peg_definition_add_rule(d, "a", count, &negs, &not_a) == MTKX_OK);
      not_a->must_fail = true;
      CHECK(mtkx_peg_definition_add_rule(d, "a", NULL, NULL, &a) == MTKX_OK);
      CHECK(mtkx_peg_definition_add_rule(d, "a.", count, &follow, &a_follow) == MTKX_OK);
      a->if_succeed = a_follow;
      CHECK(mtkx_peg_definition_parse(d, "xax", 3).success);
      CHECK(negs == 0 && follow == 1);
      not_a->fire_on_neg = true;
      CHECK(mtkx_peg_definition_parse(d, "xax", 3).success);
      CHECK(negs == 2 && follow == 2);
      mtkx_peg_definition_destroy(d);
    }

    /* --- zero-width positive matches are refused --- */
    {
      mtkx_peg_definition *d = mtkx_peg_definition_create();
      CHECK(mtkx_peg_definition_add_rule(d, "a*", NULL, NULL, NULL) == MTKX_OK);
      r = mtkx_peg_definition_parse(d, "b", 1);
      CHECK(!r.success && strstr(r.message, "refusing to loop") != NULL);
      mtkx_peg_definition_destroy(d);
    }

    /* --- PEG combinators --- */
    {
      mtkx_parser *word = mtkx_peg_rule_parser(ident);
      mtkx_parser *num = mtkx_peg_rule_parser(number);
      mtkx_parser *ws = mtkx_peg_rule_parser(space);
      mtkx_parser *pair = mtkx_peg_seq(3, mtkx_parser_retain(word), mtkx_parser_retain(ws),
                                       mtkx_parser_retain(num));
      mtkx_parser *either = mtkx_peg_choice(2, mtkx_parser_retain(num),
                                            mtkx_parser_retain(word));
      mtkx_parser *many = mtkx_peg_many(mtkx_peg_seq(2, mtkx_parser_retain(word),
                                                     mtkx_peg_opt(mtkx_parser_retain(ws))));
      mtkx_parser *many1 = mtkx_peg_many1(mtkx_parser_retain(num));
      mtkx_parser *look = mtkx_peg_seq(2, mtkx_peg_and(mtkx_parser_retain(word)),
                                       mtkx_parser_retain(word));
      mtkx_parser *neg = mtkx_peg_seq(2, mtkx_peg_not(mtkx_parser_retain(num)),
                                      mtkx_parser_retain(word));
      mtkx_parser *tagged = mtkx_peg_channel("@WORD", mtkx_parser_retain(word));
      mtkx_parse_result res;

      res = mtkx_run_parser(pair, "ab 12", 5);
      CHECK(res.ok && res.value->kind == MTKX_PV_MATCH);
      CHECK(mtkx_strview_equal_cstr(res.value->as.match.value, "ab 12"));
      CHECK(res.value->as.match.begin == 0 && res.value->as.match.end == 5);
      mtkx_parse_result_free(&res);

      res = mtkx_run_parser(either, "abc", 3);
      CHECK(res.ok && res.value->as.match.rule == ident);
      mtkx_parse_result_free(&res);
      res = mtkx_run_parser(either, "77", 2);
      CHECK(res.ok && res.value->as.match.rule == number);
      CHECK_STR_EQ(res.value->as.match.channel, "@NUM");
      mtkx_parse_result_free(&res);

      res = mtkx_run_parser(many, "a b c", 5);
      CHECK(res.ok && res.value->kind == MTKX_PV_LIST && res.value->as.list.count == 3);
      mtkx_parse_result_free(&res);

      res = mtkx_run_parser(many1, "1", 1);
      CHECK(res.ok && res.value->kind == MTKX_PV_PAIR);
      CHECK(res.value->as.pair.second->as.list.count == 0);
      mtkx_parse_result_free(&res);
      res = mtkx_run_parser(many1, "", 0);
      CHECK(!res.ok);
      mtkx_parse_result_free(&res);

      res = mtkx_run_parser(look, "abc", 3);
      CHECK(res.ok && res.value->as.match.end == 3);
      mtkx_parse_result_free(&res);

      res = mtkx_run_parser(neg, "abc", 3);
      CHECK(res.ok);
      mtkx_parse_result_free(&res);
      res = mtkx_run_parser(neg, "12", 2);
      CHECK(!res.ok && mtkx_parse_error_expects(&res.error, "negative lookahead"));
      mtkx_parse_result_free(&res);

      res = mtkx_run_parser(tagged, "\nxy", 3);
      CHECK(!res.ok);
      mtkx_parse_result_free(&res);
      res = mtkx_run_parser(tagged, "xy", 2);
      CHECK(res.ok && strcmp(res.value->as.match.channel, "@WORD") == 0);
      CHECK(!res.value->as.match.ignored && res.value->as.match.line == 1);
      mtkx_parse_result_free(&res);
      CHECK(mtkx_peg_channel("WORD", mtkx_parser_retain(word)) == NULL);

      mtkx_parser_release(word);
      mtkx_parser_release(num);
      mtkx_parser_release(ws);
      mtkx_parser_release(pair);
      mtkx_parser_release(either);
      mtkx_parser_release(many);
      mtkx_parser_release(many1);
      mtkx_parser_release(look);
      mtkx_parser_release(neg);
      mtkx_parser_release(tagged);
    }
    mtkx_peg_definition_destroy(peg);
  }
  return 0;
}
