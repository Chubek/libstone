/* AST construction, dumps, equality and rewrite sets. */
#include <memtkx/memtkx.h>

#include "test_support.h"

static bool is_identity(const mtkx_ast_node *n, void *ctx) {
  (void)ctx;
  return mtkx_ast_tag_is(n, "identity") && mtkx_ast_size(n) == 1;
}
static mtkx_ast_node *unwrap_child(const mtkx_ast_node *n, void *ctx) {
  (void)ctx;
  return mtkx_ast_clone(mtkx_ast_child(n, 0));
}

static bool is_double_not(const mtkx_ast_node *n, void *ctx) {
  (void)ctx;
  return mtkx_ast_tag_is(n, "not") && mtkx_ast_size(n) == 1 &&
         mtkx_ast_tag_is(mtkx_ast_child(n, 0), "not") &&
         mtkx_ast_size(mtkx_ast_child(n, 0)) == 1;
}
static mtkx_ast_node *drop_double_not(const mtkx_ast_node *n, void *ctx) {
  (void)ctx;
  return mtkx_ast_clone(mtkx_ast_child(mtkx_ast_child(n, 0), 0));
}

static bool always(const mtkx_ast_node *n, void *ctx) {
  (void)n;
  ++*(int *)ctx;
  return true;
}
static mtkx_ast_node *same(const mtkx_ast_node *n, void *ctx) {
  (void)ctx;
  return mtkx_ast_clone(n);
}

static char *dump(const mtkx_ast_node *n) {
  char *text = mtkx_ast_dump(n);
  CHECK(text != NULL);
  return text;
}

int main(void) {
  char *text;
  mtkx_ast_node *expr = mtkx_ast_node_of("add", 2, mtkx_ast_leaf_int("num", 1),
                                         mtkx_ast_leaf_int("num", 2));
  mtkx_ast_node *copy;
  CHECK(expr != NULL);
  text = dump(expr);
  CHECK_STR_EQ(text, "(add (num 1) (num 2))");
  mtkx_free(text);

  CHECK(!mtkx_ast_is_leaf(expr) && mtkx_ast_size(expr) == 2);
  CHECK_STR_EQ(mtkx_ast_value(mtkx_ast_child(expr, 1)), "2");
  CHECK(mtkx_ast_child(expr, 5) == NULL);
  copy = mtkx_ast_clone(expr);
  CHECK(mtkx_ast_equal(expr, copy));
  CHECK(mtkx_ast_append_child(copy, mtkx_ast_leaf_double("num", 2.5)) == MTKX_OK);
  CHECK(!mtkx_ast_equal(expr, copy));
  text = dump(copy);
  CHECK_STR_EQ(text, "(add (num 1) (num 2) (num 2.5))");
  mtkx_free(text);
  {
    mtkx_ast_node *taken = mtkx_ast_take_child(copy, 0);
    CHECK(taken != NULL && mtkx_ast_size(copy) == 2);
    CHECK(mtkx_ast_append_child(mtkx_ast_child(copy, 0), taken) ==
          MTKX_ERR_INVALID_ARGUMENT); /* leaves have no children; `taken` freed */
  }
  mtkx_ast_free(copy);
  mtkx_ast_free(expr);

  /* empty leaves, formatted leaves, NULL propagation */
  {
    mtkx_ast_node *e = mtkx_ast_empty();
    mtkx_ast_node *f = mtkx_ast_leaf_fmt("pt", "%d,%d", 3, 4);
    mtkx_ast_node *n;
    text = dump(e);
    CHECK_STR_EQ(text, "()");
    mtkx_free(text);
    text = dump(f);
    CHECK_STR_EQ(text, "(pt 3,4)");
    mtkx_free(text);
    mtkx_ast_free(e);
    mtkx_ast_free(f);
    n = mtkx_ast_node_of("bad", 2, mtkx_ast_leaf("x", "1"), (mtkx_ast_node *)NULL);
    CHECK(n == NULL);
    n = mtkx_ast_node_of("select", 3, mtkx_ast_leaf("col", "name"), mtkx_ast_leaf("col", "age"),
                         mtkx_ast_leaf("from", "users"));
    text = dump(n);
    CHECK_STR_EQ(text, "(select (col name) (col age) (from users))");
    mtkx_free(text);
    mtkx_ast_free(n);
  }

  /* guide 12.4: remove-identity */
  {
    mtkx_ast_node *tree = mtkx_ast_node_of("identity", 1, mtkx_ast_leaf_int("number", 42));
    mtkx_rewrite_rule rules[1];
    mtkx_rewrite_set set;
    mtkx_ast_node *simplified;
    rules[0] = mtkx_rewrite_rule_make("remove-identity", is_identity, unwrap_child, NULL);
    set = mtkx_rewrite_set_make(rules, 1);
    simplified = mtkx_rewrite_apply(&set, tree);
    text = dump(simplified);
    CHECK_STR_EQ(text, "(number 42)");
    mtkx_free(text);
    text = dump(tree); /* the input is untouched */
    CHECK_STR_EQ(text, "(identity (number 42))");
    mtkx_free(text);
    mtkx_ast_free(simplified);
    mtkx_ast_free(tree);
  }

  /* nested double negation reaches a fixpoint over several passes */
  {
    mtkx_ast_node *tree = mtkx_ast_node_of(
        "and", 2,
        mtkx_ast_node_of("not", 1,
                         mtkx_ast_node_of("not", 1,
                                          mtkx_ast_node_of("not", 1,
                                                           mtkx_ast_node_of("not", 1,
                                                                            mtkx_ast_leaf("var", "x"))))),
        mtkx_ast_leaf("var", "y"));
    mtkx_rewrite_rule rules[1];
    mtkx_rewrite_set set;
    mtkx_ast_node *out;
    rules[0] = mtkx_rewrite_rule_make("double-neg", is_double_not, drop_double_not, NULL);
    set = mtkx_rewrite_set_make(rules, 1);
    out = mtkx_rewrite_apply(&set, tree);
    text = dump(out);
    CHECK_STR_EQ(text, "(and (var x) (var y))");
    mtkx_free(text);
    mtkx_ast_free(out);
    mtkx_ast_free(tree);
  }

  /* a rule that always fires is bounded by max_iterations */
  {
    mtkx_ast_node *tree = mtkx_ast_leaf("x", "1");
    mtkx_rewrite_rule rules[1];
    mtkx_rewrite_set set;
    mtkx_ast_node *out;
    int fired = 0;
    rules[0] = mtkx_rewrite_rule_make("forever", always, same, &fired);
    set = mtkx_rewrite_set_make(rules, 1);
    set.max_iterations = 7;
    out = mtkx_rewrite_apply(&set, tree);
    CHECK(out != NULL && fired == 7);
    mtkx_ast_free(out);
    mtkx_ast_free(tree);
  }
  return 0;
}
