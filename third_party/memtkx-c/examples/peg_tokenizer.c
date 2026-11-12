/* GUIDE 12.8: a PEG tokenizer with an ignore channel. */
#include <memtkx/dsl.h>

#include <assert.h>

static void count(mtkx_peg_match *match, void *ctx) {
  (void)match;
  ++*(int *)ctx;
}

int main(void) {
  int idents = 0;
  mtkx_peg_definition *peg = mtkx_peg_definition_create();
  mtkx_peg_rule *space = NULL;
  mtkx_peg_parse_result r;

  mtkx_peg_definition_add_rule(peg, "[a-z][a-z0-9]*", count, &idents, NULL);
  mtkx_peg_definition_add_rule(peg, "[0-9]+", NULL, NULL, NULL);
  mtkx_peg_definition_add_rule(peg, "[ ]+", NULL, NULL, &space);
  mtkx_peg_rule_set_channel(space, MTKX_PEG_IGNORE_CHANNEL);

  r = mtkx_peg_definition_parse(peg, "ab 12 c3", 8);
  assert(r.success && idents == 2);
  r = mtkx_peg_definition_parse(peg, "ab #", 4);
  assert(!r.success && r.offset == 3 && r.column == 4);
  mtkx_peg_definition_destroy(peg);
  return 0;
}
