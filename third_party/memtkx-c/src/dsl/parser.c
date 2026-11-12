#include "memtkx/dsl/parser.h"

#include <stdio.h>
#include <string.h>

#include "internal/common.h"
#include "internal/strbuf.h"

/* ------------------------------------------------------------------ */
/*  Errors                                                            */
/* ------------------------------------------------------------------ */

static void mtkx__parse_error_free_labels(mtkx_parse_error *error) {
  size_t i;
  for (i = 0; i < error->expected_count; ++i) {
    mtkx_free(error->expected[i]);
  }
  mtkx_free(error->expected);
  error->expected = NULL;
  error->expected_count = 0;
}

void mtkx_parse_error_clear(mtkx_parse_error *error) {
  mtkx__parse_error_free_labels(error);
  error->pos = 0;
  error->kind = MTKX_PARSE_SOFT;
  error->out_of_memory = false;
}

mtkx_status mtkx_parse_error_add_expected(mtkx_parse_error *error, const char *label) {
  char *copy = mtkx_strdup(label);
  char **grown;
  if (copy == NULL) {
    error->out_of_memory = true;
    return MTKX_ERR_NOMEM;
  }
  grown = (char **)mtkx_realloc(error->expected,
                                (error->expected_count + 1) * sizeof *error->expected);
  if (grown == NULL) {
    mtkx_free(copy);
    error->out_of_memory = true;
    return MTKX_ERR_NOMEM;
  }
  error->expected = grown;
  error->expected[error->expected_count++] = copy;
  return MTKX_OK;
}

mtkx_status mtkx_parse_error_set_expected(mtkx_parse_error *error, const char *label) {
  mtkx__parse_error_free_labels(error);
  return mtkx_parse_error_add_expected(error, label);
}

bool mtkx_parse_error_expects(const mtkx_parse_error *error, const char *label) {
  size_t i;
  for (i = 0; i < error->expected_count; ++i) {
    if (strcmp(error->expected[i], label) == 0) {
      return true;
    }
  }
  return false;
}

mtkx_status mtkx_parse_error_copy(mtkx_parse_error *dst, const mtkx_parse_error *src) {
  size_t i;
  mtkx_parse_error_clear(dst);
  dst->pos = src->pos;
  dst->kind = src->kind;
  dst->out_of_memory = src->out_of_memory;
  for (i = 0; i < src->expected_count; ++i) {
    if (mtkx_parse_error_add_expected(dst, src->expected[i]) != MTKX_OK) {
      return MTKX_ERR_NOMEM;
    }
  }
  return MTKX_OK;
}

void mtkx_parse_error_merge(mtkx_parse_error *dst, const mtkx_parse_error *src) {
  size_t i;
  bool oom = dst->out_of_memory || src->out_of_memory;
  if (src->pos > dst->pos) {
    (void)mtkx_parse_error_copy(dst, src);
    dst->out_of_memory = dst->out_of_memory || oom;
    return;
  }
  dst->out_of_memory = oom;
  if (src->pos < dst->pos) {
    return;
  }
  if (src->kind == MTKX_PARSE_COMMITTED) {
    dst->kind = MTKX_PARSE_COMMITTED;
  }
  for (i = 0; i < src->expected_count; ++i) {
    if (!mtkx_parse_error_expects(dst, src->expected[i])) {
      (void)mtkx_parse_error_add_expected(dst, src->expected[i]);
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Values                                                            */
/* ------------------------------------------------------------------ */

static mtkx_parse_value *mtkx__value_new(mtkx_parse_value_kind kind) {
  mtkx_parse_value *v = (mtkx_parse_value *)mtkx_calloc(1, sizeof *v);
  if (v != NULL) {
    v->kind = kind;
  }
  return v;
}

mtkx_parse_value *mtkx_parse_value_char(char ch) {
  mtkx_parse_value *v = mtkx__value_new(MTKX_PV_CHAR);
  if (v != NULL) {
    v->as.ch = ch;
  }
  return v;
}

mtkx_parse_value *mtkx_parse_value_bool(bool value) {
  mtkx_parse_value *v = mtkx__value_new(MTKX_PV_BOOL);
  if (v != NULL) {
    v->as.boolean = value;
  }
  return v;
}

mtkx_parse_value *mtkx_parse_value_pair(mtkx_parse_value *first, mtkx_parse_value *second) {
  mtkx_parse_value *v;
  if (first == NULL || second == NULL) {
    mtkx_parse_value_free(first);
    mtkx_parse_value_free(second);
    return NULL;
  }
  v = mtkx__value_new(MTKX_PV_PAIR);
  if (v == NULL) {
    mtkx_parse_value_free(first);
    mtkx_parse_value_free(second);
    return NULL;
  }
  v->as.pair.first = first;
  v->as.pair.second = second;
  return v;
}

mtkx_parse_value *mtkx_parse_value_list(void) { return mtkx__value_new(MTKX_PV_LIST); }

mtkx_status mtkx_parse_value_list_push(mtkx_parse_value *list, mtkx_parse_value *item) {
  if (list == NULL || item == NULL || list->kind != MTKX_PV_LIST) {
    mtkx_parse_value_free(item);
    return item == NULL ? MTKX_ERR_NOMEM : MTKX_ERR_INVALID_ARGUMENT;
  }
  if (MTKX__GROW(list->as.list.items, list->as.list.capacity, list->as.list.count + 1) !=
      MTKX_OK) {
    mtkx_parse_value_free(item);
    return MTKX_ERR_NOMEM;
  }
  list->as.list.items[list->as.list.count++] = item;
  return MTKX_OK;
}

mtkx_parse_value *mtkx_parse_value_none(void) { return mtkx__value_new(MTKX_PV_OPTIONAL); }

mtkx_parse_value *mtkx_parse_value_some(mtkx_parse_value *inner) {
  mtkx_parse_value *v;
  if (inner == NULL) {
    return NULL;
  }
  v = mtkx__value_new(MTKX_PV_OPTIONAL);
  if (v == NULL) {
    mtkx_parse_value_free(inner);
    return NULL;
  }
  v->as.optional = inner;
  return v;
}

mtkx_parse_value *mtkx_parse_value_match(const mtkx_peg_match *match) {
  mtkx_parse_value *v = mtkx__value_new(MTKX_PV_MATCH);
  if (v != NULL) {
    v->as.match = *match;
  }
  return v;
}

mtkx_parse_value *mtkx_parse_value_user(void *ptr, void (*destroy)(void *)) {
  mtkx_parse_value *v = mtkx__value_new(MTKX_PV_USER);
  if (v == NULL) {
    if (destroy != NULL) {
      destroy(ptr);
    }
    return NULL;
  }
  v->as.user.ptr = ptr;
  v->as.user.destroy = destroy;
  return v;
}

void mtkx_parse_value_free(mtkx_parse_value *value) {
  size_t i;
  if (value == NULL) {
    return;
  }
  switch (value->kind) {
    case MTKX_PV_PAIR:
      mtkx_parse_value_free(value->as.pair.first);
      mtkx_parse_value_free(value->as.pair.second);
      break;
    case MTKX_PV_LIST:
      for (i = 0; i < value->as.list.count; ++i) {
        mtkx_parse_value_free(value->as.list.items[i]);
      }
      mtkx_free(value->as.list.items);
      break;
    case MTKX_PV_OPTIONAL:
      mtkx_parse_value_free(value->as.optional);
      break;
    case MTKX_PV_USER:
      if (value->as.user.destroy != NULL) {
        value->as.user.destroy(value->as.user.ptr);
      }
      break;
    case MTKX_PV_CHAR:
    case MTKX_PV_BOOL:
    case MTKX_PV_MATCH:
      break;
  }
  mtkx_free(value);
}

/* ------------------------------------------------------------------ */
/*  Results                                                           */
/* ------------------------------------------------------------------ */

static mtkx_parse_result mtkx__result_empty(void) {
  mtkx_parse_result r;
  memset(&r, 0, sizeof r);
  return r;
}

mtkx_parse_result mtkx_parse_fail(size_t pos, mtkx_parse_failure_kind kind,
                                  const char *label) {
  mtkx_parse_result r = mtkx__result_empty();
  r.ok = false;
  r.error.pos = pos;
  r.error.kind = kind;
  if (label != NULL) {
    (void)mtkx_parse_error_add_expected(&r.error, label);
  }
  if (r.error.out_of_memory) {
    r.error.kind = MTKX_PARSE_COMMITTED;
  }
  return r;
}

static mtkx_parse_result mtkx__result_oom(size_t pos) {
  mtkx_parse_result r = mtkx_parse_fail(pos, MTKX_PARSE_COMMITTED, NULL);
  r.error.out_of_memory = true;
  return r;
}

mtkx_parse_result mtkx_parse_ok(mtkx_parse_value *value) {
  mtkx_parse_result r;
  if (value == NULL) {
    return mtkx__result_oom(0);
  }
  r = mtkx__result_empty();
  r.ok = true;
  r.value = value;
  return r;
}

mtkx_parse_result mtkx_parse_fail_expected(const mtkx_parse_input *in, const char *label,
                                           mtkx_parse_failure_kind kind) {
  return mtkx_parse_fail(in->pos, kind, label);
}

mtkx_parse_result mtkx_parse_fail_with(mtkx_parse_error *moved_error) {
  mtkx_parse_result r = mtkx__result_empty();
  r.ok = false;
  r.error = *moved_error;
  memset(moved_error, 0, sizeof *moved_error);
  if (r.error.out_of_memory) {
    r.error.kind = MTKX_PARSE_COMMITTED;
  }
  return r;
}

mtkx_parse_value *mtkx_parse_result_take_value(mtkx_parse_result *result) {
  mtkx_parse_value *v = result->value;
  result->value = NULL;
  return v;
}

void mtkx_parse_result_free(mtkx_parse_result *result) {
  mtkx_parse_value_free(result->value);
  result->value = NULL;
  mtkx_parse_error_clear(&result->error);
}

/* ------------------------------------------------------------------ */
/*  Parser objects                                                    */
/* ------------------------------------------------------------------ */

struct mtkx_parser {
  size_t refcount;
  mtkx_parse_fn fn;
  void *ctx;
  void (*ctx_free)(void *ctx);
};

mtkx_parser *mtkx_parser_new(mtkx_parse_fn fn, void *ctx, void (*ctx_free)(void *ctx)) {
  mtkx_parser *p = (mtkx_parser *)mtkx_malloc(sizeof *p);
  if (p == NULL || fn == NULL) {
    mtkx_free(p);
    if (ctx_free != NULL) {
      ctx_free(ctx);
    }
    return NULL;
  }
  p->refcount = 1;
  p->fn = fn;
  p->ctx = ctx;
  p->ctx_free = ctx_free;
  return p;
}

mtkx_parser *mtkx_parser_retain(mtkx_parser *parser) {
  if (parser != NULL) {
    ++parser->refcount;
  }
  return parser;
}

void mtkx_parser_release(mtkx_parser *parser) {
  if (parser == NULL || --parser->refcount != 0) {
    return;
  }
  if (parser->ctx_free != NULL) {
    parser->ctx_free(parser->ctx);
  }
  mtkx_free(parser);
}

mtkx_parse_result mtkx_parser_parse(const mtkx_parser *parser, mtkx_parse_input *in) {
  if (parser == NULL) {
    return mtkx_parse_fail(in->pos, MTKX_PARSE_COMMITTED, "<null parser>");
  }
  return parser->fn(in, parser->ctx);
}

/* ------------------------------------------------------------------ */
/*  Combinator contexts                                               */
/* ------------------------------------------------------------------ */

typedef struct mtkx__combinator {
  mtkx_parser *a;
  mtkx_parser *b;
  char *label;
} mtkx__combinator;

static void mtkx__combinator_free(void *ctx) {
  mtkx__combinator *c = (mtkx__combinator *)ctx;
  mtkx_parser_release(c->a);
  mtkx_parser_release(c->b);
  mtkx_free(c->label);
  mtkx_free(c);
}

/*
 * Builds a combinator parser. Consumes a and b (either may be NULL when
 * `arity` is 1 for b). Returns NULL, releasing operands, on failure.
 */
static mtkx_parser *mtkx__combinator_new(mtkx_parse_fn fn, mtkx_parser *a, mtkx_parser *b,
                                         int arity, const char *label) {
  mtkx__combinator *c;
  if (a == NULL || (arity == 2 && b == NULL)) {
    mtkx_parser_release(a);
    mtkx_parser_release(b);
    return NULL;
  }
  c = (mtkx__combinator *)mtkx_calloc(1, sizeof *c);
  if (c == NULL) {
    mtkx_parser_release(a);
    mtkx_parser_release(b);
    return NULL;
  }
  c->a = a;
  c->b = b;
  if (label != NULL) {
    c->label = mtkx_strdup(label);
    if (c->label == NULL) {
      mtkx__combinator_free(c);
      return NULL;
    }
  }
  return mtkx_parser_new(fn, c, mtkx__combinator_free);
}

/* ------------------------------------------------------------------ */
/*  Primitive parsers                                                 */
/* ------------------------------------------------------------------ */

typedef struct mtkx__ch_ctx {
  char c;
  char label[2];
} mtkx__ch_ctx;

static mtkx_parse_result mtkx__ch_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__ch_ctx *c = (const mtkx__ch_ctx *)ctx;
  if (mtkx_parse_input_peek(in) == c->c) {
    size_t pos = in->pos;
    mtkx_parse_result r = mtkx_parse_ok(mtkx_parse_value_char(mtkx_parse_input_consume(in)));
    if (!r.ok) {
      r.error.pos = pos;
    }
    return r;
  }
  return mtkx_parse_fail_expected(in, c->label, MTKX_PARSE_SOFT);
}

mtkx_parser *mtkx_parser_ch(char c) {
  mtkx__ch_ctx *ctx = (mtkx__ch_ctx *)mtkx_malloc(sizeof *ctx);
  if (ctx == NULL) {
    return NULL;
  }
  ctx->c = c;
  ctx->label[0] = c;
  ctx->label[1] = '\0';
  return mtkx_parser_new(mtkx__ch_parse, ctx, mtkx_free);
}

typedef struct mtkx__satisfy_ctx {
  mtkx_char_pred_fn pred;
  void *pred_ctx;
  char *label;
} mtkx__satisfy_ctx;

static void mtkx__satisfy_free(void *ctx) {
  mtkx__satisfy_ctx *s = (mtkx__satisfy_ctx *)ctx;
  mtkx_free(s->label);
  mtkx_free(s);
}

static mtkx_parse_result mtkx__satisfy_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__satisfy_ctx *s = (const mtkx__satisfy_ctx *)ctx;
  if (!mtkx_parse_input_eof(in) && s->pred(mtkx_parse_input_peek(in), s->pred_ctx)) {
    return mtkx_parse_ok(mtkx_parse_value_char(mtkx_parse_input_consume(in)));
  }
  return mtkx_parse_fail_expected(in, s->label, MTKX_PARSE_SOFT);
}

mtkx_parser *mtkx_parser_satisfy(mtkx_char_pred_fn pred, void *ctx, const char *label) {
  mtkx__satisfy_ctx *s;
  if (pred == NULL) {
    return NULL;
  }
  s = (mtkx__satisfy_ctx *)mtkx_malloc(sizeof *s);
  if (s == NULL) {
    return NULL;
  }
  s->pred = pred;
  s->pred_ctx = ctx;
  s->label = mtkx_strdup(label != NULL ? label : "");
  if (s->label == NULL) {
    mtkx_free(s);
    return NULL;
  }
  return mtkx_parser_new(mtkx__satisfy_parse, s, mtkx__satisfy_free);
}

/* ------------------------------------------------------------------ */
/*  Combinators                                                       */
/* ------------------------------------------------------------------ */

static mtkx_parse_result mtkx__seq_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__combinator *c = (const mtkx__combinator *)ctx;
  size_t save = in->pos;
  mtkx_parse_result ra = mtkx_parser_parse(c->a, in);
  mtkx_parse_result rb;
  if (!ra.ok) {
    return ra;
  }
  rb = mtkx_parser_parse(c->b, in);
  if (!rb.ok) {
    bool consumed = in->pos != save;
    in->pos = save;
    if (consumed) {
      rb.error.kind = MTKX_PARSE_COMMITTED;
    }
    mtkx_parse_result_free(&ra);
    return rb;
  }
  return mtkx_parse_ok(mtkx_parse_value_pair(mtkx_parse_result_take_value(&ra),
                                             mtkx_parse_result_take_value(&rb)));
}

mtkx_parser *mtkx_parser_seq(mtkx_parser *a, mtkx_parser *b) {
  return mtkx__combinator_new(mtkx__seq_parse, a, b, 2, NULL);
}

static mtkx_parse_result mtkx__alt_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__combinator *c = (const mtkx__combinator *)ctx;
  size_t save = in->pos;
  mtkx_parse_result ra = mtkx_parser_parse(c->a, in);
  mtkx_parse_result rb;
  if (ra.ok || ra.error.kind == MTKX_PARSE_COMMITTED) {
    return ra;
  }
  in->pos = save;
  rb = mtkx_parser_parse(c->b, in);
  if (!rb.ok) {
    mtkx_parse_error_merge(&rb.error, &ra.error);
  }
  mtkx_parse_result_free(&ra);
  return rb;
}

mtkx_parser *mtkx_parser_alt(mtkx_parser *a, mtkx_parser *b) {
  return mtkx__combinator_new(mtkx__alt_parse, a, b, 2, NULL);
}

static mtkx_parse_result mtkx__many_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__combinator *c = (const mtkx__combinator *)ctx;
  mtkx_parse_value *out = mtkx_parse_value_list();
  mtkx_parse_error best;
  if (out == NULL) {
    return mtkx__result_oom(in->pos);
  }
  memset(&best, 0, sizeof best);
  for (;;) {
    size_t save = in->pos;
    mtkx_parse_result r = mtkx_parser_parse(c->a, in);
    if (!r.ok) {
      mtkx_parse_error_merge(&best, &r.error);
      if (r.error.kind == MTKX_PARSE_COMMITTED) {
        mtkx_parse_result_free(&r);
        mtkx_parse_value_free(out);
        return mtkx_parse_fail_with(&best);
      }
      in->pos = save;
      mtkx_parse_result_free(&r);
      break;
    }
    if (mtkx_parse_value_list_push(out, mtkx_parse_result_take_value(&r)) != MTKX_OK) {
      mtkx_parse_error_clear(&best);
      mtkx_parse_value_free(out);
      return mtkx__result_oom(in->pos);
    }
    mtkx_parse_result_free(&r);
    /* A success that consumed nothing would repeat forever (the C++
     * operator* hung here); stop instead. */
    if (in->pos == save) {
      break;
    }
  }
  mtkx_parse_error_clear(&best);
  return mtkx_parse_ok(out);
}

mtkx_parser *mtkx_parser_many(mtkx_parser *p) {
  return mtkx__combinator_new(mtkx__many_parse, p, NULL, 1, NULL);
}

static mtkx_parse_result mtkx__optional_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__combinator *c = (const mtkx__combinator *)ctx;
  size_t save = in->pos;
  mtkx_parse_result r = mtkx_parser_parse(c->a, in);
  if (!r.ok) {
    if (r.error.kind == MTKX_PARSE_COMMITTED) {
      return r;
    }
    in->pos = save;
    mtkx_parse_result_free(&r);
    return mtkx_parse_ok(mtkx_parse_value_none());
  }
  return mtkx_parse_ok(mtkx_parse_value_some(mtkx_parse_result_take_value(&r)));
}

mtkx_parser *mtkx_parser_optional(mtkx_parser *p) {
  return mtkx__combinator_new(mtkx__optional_parse, p, NULL, 1, NULL);
}

static mtkx_parse_result mtkx__try_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__combinator *c = (const mtkx__combinator *)ctx;
  size_t save = in->pos;
  mtkx_parse_result r = mtkx_parser_parse(c->a, in);
  if (!r.ok) {
    in->pos = save;
    if (!r.error.out_of_memory) {
      r.error.kind = MTKX_PARSE_SOFT;
    }
  }
  return r;
}

mtkx_parser *mtkx_parser_try(mtkx_parser *p) {
  return mtkx__combinator_new(mtkx__try_parse, p, NULL, 1, NULL);
}

static mtkx_parse_result mtkx__labeled_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__combinator *c = (const mtkx__combinator *)ctx;
  mtkx_parse_result r = mtkx_parser_parse(c->a, in);
  if (!r.ok) {
    (void)mtkx_parse_error_set_expected(&r.error, c->label);
  }
  return r;
}

mtkx_parser *mtkx_parser_labeled(mtkx_parser *p, const char *label) {
  return mtkx__combinator_new(mtkx__labeled_parse, p, NULL, 1, label != NULL ? label : "");
}

mtkx_parse_result mtkx_run_parser(const mtkx_parser *parser, const char *source,
                                  size_t length) {
  mtkx_parse_input in = mtkx_parse_input_make(source != NULL ? source : "",
                                              source != NULL ? length : 0);
  mtkx_parse_result r = mtkx_parser_parse(parser, &in);
  if (!r.ok) {
    return r;
  }
  if (!mtkx_parse_input_eof(&in)) {
    mtkx_parse_result_free(&r);
    return mtkx_parse_fail(in.pos, MTKX_PARSE_COMMITTED, "<end-of-input>");
  }
  return r;
}

/* ------------------------------------------------------------------ */
/*  Semantic actions                                                  */
/* ------------------------------------------------------------------ */

bool mtkx_parse_with_action(const mtkx_parser *parser, mtkx_parse_input *in,
                            const mtkx_semantic_entry *semantics, size_t count,
                            mtkx_parse_action_fn action, void *ctx, void *out) {
  size_t begin = in->pos;
  mtkx_parse_result r = mtkx_parser_parse(parser, in);
  mtkx_parser_stage stage;
  bool ok = r.ok;
  mtkx_parse_result_free(&r);
  if (!ok) {
    return false;
  }
  stage.begin = begin;
  stage.end = in->pos;
  stage.span = mtkx_parse_input_span(in, begin);
  if (action != NULL) {
    action(semantics, count, &stage, out, ctx);
  }
  return true;
}

const mtkx_production *mtkx_production_find(const mtkx_production *productions,
                                            size_t count, const char *id) {
  size_t i;
  for (i = 0; i < count; ++i) {
    if (productions[i].id != NULL && id != NULL && strcmp(productions[i].id, id) == 0) {
      return &productions[i];
    }
  }
  return NULL;
}

/* ------------------------------------------------------------------ */
/*  File input                                                        */
/* ------------------------------------------------------------------ */

static char *mtkx__file_message(const char *what, const char *path) {
  mtkx__strbuf sb;
  mtkx__strbuf_init(&sb);
  mtkx__strbuf_appendf(&sb, "mtkx_load_parse_input_file: %s '%s'", what,
                       path != NULL ? path : "");
  return mtkx__strbuf_finish(&sb);
}

mtkx_status mtkx_load_parse_input_file(const char *path, char **out_text,
                                       size_t *out_length, char **out_message) {
  FILE *file;
  mtkx__strbuf sb;
  char chunk[4096];
  size_t n;
  bool read_error;

  if (out_message != NULL) {
    *out_message = NULL;
  }
  if (out_text == NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  *out_text = NULL;
  if (out_length != NULL) {
    *out_length = 0;
  }
  file = path != NULL ? fopen(path, "rb") : NULL;
  if (file == NULL) {
    if (out_message != NULL) {
      *out_message = mtkx__file_message("cannot open", path);
    }
    return MTKX_ERR_IO;
  }
  mtkx__strbuf_init(&sb);
  while ((n = fread(chunk, 1, sizeof chunk, file)) != 0) {
    mtkx__strbuf_append(&sb, chunk, n);
  }
  read_error = ferror(file) != 0;
  fclose(file);
  if (read_error) {
    mtkx__strbuf_destroy(&sb);
    if (out_message != NULL) {
      *out_message = mtkx__file_message("read failed for", path);
    }
    return MTKX_ERR_IO;
  }
  n = sb.length;
  *out_text = mtkx__strbuf_finish(&sb);
  if (*out_text == NULL) {
    return MTKX_ERR_NOMEM;
  }
  if (out_length != NULL) {
    *out_length = n;
  }
  return MTKX_OK;
}
