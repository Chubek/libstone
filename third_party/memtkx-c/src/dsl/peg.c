#include "memtkx/dsl/peg.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "internal/common.h"

static const char mtkx__default_channel[] = MTKX_PEG_DEFAULT_CHANNEL;

/* ------------------------------------------------------------------ */
/*  Channels                                                          */
/* ------------------------------------------------------------------ */

mtkx_status mtkx_peg_channel_validate(const char *name) {
  return (name != NULL && name[0] == '@') ? MTKX_OK : MTKX_ERR_INVALID_ARGUMENT;
}

bool mtkx_peg_channel_is_ignore(const char *name) {
  return name != NULL && strcmp(name, MTKX_PEG_IGNORE_CHANNEL) == 0;
}

/* ------------------------------------------------------------------ */
/*  Compiled patterns (port of peg_detail::CompiledPattern)           */
/* ------------------------------------------------------------------ */

typedef struct mtkx__peg_atom {
  bool any;
  bool is_class;
  bool negate;
  char ch;
  const char *cls; /* points into the owning pattern's class storage */
  size_t cls_len;
  char quant; /* 0, '*', '+', '?', '!' */
} mtkx__peg_atom;

struct mtkx_peg_pattern {
  mtkx__peg_atom *atoms;
  size_t count;
  char *text; /* copy of the pattern; class bodies point into it */
};

static void mtkx__peg_pattern_free(mtkx_peg_pattern *p) {
  if (p == NULL) {
    return;
  }
  mtkx_free(p->atoms);
  mtkx_free(p->text);
  mtkx_free(p);
}

static const char *mtkx__peg_compile(const char *pattern, mtkx_peg_pattern **out) {
  size_t len = strlen(pattern);
  size_t i = 0;
  mtkx_peg_pattern *p = NULL;
  size_t capacity = 0;

  if (out != NULL) {
    *out = NULL;
    p = (mtkx_peg_pattern *)mtkx_calloc(1, sizeof *p);
    if (p == NULL || (p->text = mtkx_strndup(pattern, len)) == NULL) {
      mtkx__peg_pattern_free(p);
      return "out of memory";
    }
    pattern = p->text;
  }

  while (i < len) {
    mtkx__peg_atom a;
    if (pattern[i] == '^' || pattern[i] == '$') {
      ++i; /* anchors are implicit in PEG matching */
      continue;
    }
    memset(&a, 0, sizeof a);
    if (pattern[i] == '.') {
      a.any = true;
      ++i;
    } else if (pattern[i] == '[') {
      const char *close = (const char *)memchr(pattern + i + 1, ']', len - i - 1);
      if (close == NULL) {
        mtkx__peg_pattern_free(p);
        return "unterminated character class";
      }
      a.is_class = true;
      a.cls = pattern + i + 1;
      a.cls_len = (size_t)(close - a.cls);
      if (a.cls_len != 0 && a.cls[0] == '^') {
        a.negate = true;
        ++a.cls;
        --a.cls_len;
      }
      i = (size_t)(close - pattern) + 1;
    } else {
      a.ch = pattern[i];
      ++i;
    }
    if (i < len && (pattern[i] == '*' || pattern[i] == '+' || pattern[i] == '?' ||
                    pattern[i] == '!')) {
      a.quant = pattern[i++];
    }
    if (p != NULL) {
      if (MTKX__GROW(p->atoms, capacity, p->count + 1) != MTKX_OK) {
        mtkx__peg_pattern_free(p);
        return "out of memory";
      }
      p->atoms[p->count++] = a;
    }
  }
  if (out != NULL) {
    *out = p;
  }
  return NULL;
}

const char *mtkx_peg_pattern_error(const char *pattern) {
  if (pattern == NULL) {
    return "null pattern";
  }
  return mtkx__peg_compile(pattern, NULL);
}

static bool mtkx__peg_class_test(const mtkx__peg_atom *a, char c) {
  bool hit = false;
  size_t i = 0;
  while (i < a->cls_len) {
    if (i + 2 < a->cls_len && a->cls[i + 1] == '-') {
      if (c >= a->cls[i] && c <= a->cls[i + 2]) {
        hit = true;
      }
      i += 3;
    } else {
      if (a->cls[i] == c) {
        hit = true;
      }
      ++i;
    }
  }
  return a->negate ? !hit : hit;
}

static bool mtkx__peg_atom_test(const mtkx__peg_atom *a, char c) {
  if (a->any) {
    return true;
  }
  if (a->is_class) {
    return mtkx__peg_class_test(a, c);
  }
  return a->ch == c;
}

/* Greedy prefix match at `start`; returns true and the end offset. */
static bool mtkx__peg_match_at(const mtkx_peg_pattern *p, const char *text, size_t length,
                               size_t start, size_t *end) {
  size_t pos = start;
  size_t k;
  for (k = 0; k < p->count; ++k) {
    const mtkx__peg_atom *a = &p->atoms[k];
    switch (a->quant) {
      case '*':
        while (pos < length && mtkx__peg_atom_test(a, text[pos])) {
          ++pos;
        }
        break;
      case '+': {
        size_t n = 0;
        while (pos < length && mtkx__peg_atom_test(a, text[pos])) {
          ++pos;
          ++n;
        }
        if (n == 0) {
          return false;
        }
        break;
      }
      case '?':
        if (pos < length && mtkx__peg_atom_test(a, text[pos])) {
          ++pos;
        }
        break;
      case '!':
        /* must-fail atom: zero-width negative lookahead */
        if (pos < length && mtkx__peg_atom_test(a, text[pos])) {
          return false;
        }
        break;
      default:
        if (pos >= length || !mtkx__peg_atom_test(a, text[pos])) {
          return false;
        }
        ++pos;
        break;
    }
  }
  *end = pos;
  return true;
}

static void mtkx__line_col_at(const char *text, size_t length, size_t pos, size_t *line,
                              size_t *col) {
  size_t i;
  *line = 1;
  *col = 1;
  for (i = 0; i < pos && i < length; ++i) {
    if (text[i] == '\n') {
      ++*line;
      *col = 1;
    } else {
      ++*col;
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Rules                                                             */
/* ------------------------------------------------------------------ */

static void mtkx__peg_rule_free(mtkx_peg_rule *rule) {
  if (rule == NULL) {
    return;
  }
  mtkx_free(rule->pattern_text);
  mtkx__peg_pattern_free(rule->compiled);
  mtkx_free(rule->channel);
  mtkx_free(rule);
}

mtkx_status mtkx_peg_rule_set_channel(mtkx_peg_rule *rule, const char *name) {
  char *copy;
  if (mtkx_peg_channel_validate(name) != MTKX_OK) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  copy = mtkx_strdup(name);
  if (copy == NULL) {
    return MTKX_ERR_NOMEM;
  }
  mtkx_free(rule->channel);
  rule->channel = copy;
  return MTKX_OK;
}

void mtkx_peg_rule_set_action(mtkx_peg_rule *rule, mtkx_peg_action_fn fn, void *ctx) {
  rule->semantic_action.fn = fn;
  rule->semantic_action.ctx = ctx;
}

static const char *mtkx__rule_channel(const mtkx_peg_rule *rule) {
  return rule->channel != NULL ? rule->channel : mtkx__default_channel;
}

bool mtkx_peg_rule_try_match(const mtkx_peg_rule *rule, const char *text, size_t length,
                             size_t pos, size_t *end) {
  size_t e;
  if (rule->must_fail) {
    if (mtkx__peg_match_at(rule->compiled, text, length, pos, &e)) {
      return false;
    }
    if (pos < length) {
      *end = pos + 1;
      return true;
    }
    return false;
  }
  if (!mtkx__peg_match_at(rule->compiled, text, length, pos, &e)) {
    return false;
  }
  *end = e;
  return true;
}

static mtkx_peg_match mtkx__make_match(const char *text, size_t length, size_t begin,
                                       size_t end, const mtkx_peg_rule *rule) {
  mtkx_peg_match m;
  memset(&m, 0, sizeof m);
  m.value = mtkx_strview_make(text + begin, end - begin);
  m.begin = begin;
  m.end = end;
  m.rule = (mtkx_peg_rule *)rule;
  m.channel = rule != NULL ? mtkx__rule_channel(rule) : mtkx__default_channel;
  m.ignored = mtkx_peg_channel_is_ignore(m.channel);
  mtkx__line_col_at(text, length, begin, &m.line, &m.column);
  return m;
}

/* ------------------------------------------------------------------ */
/*  Definitions                                                       */
/* ------------------------------------------------------------------ */

struct mtkx_peg_definition {
  const mtkx_peg_definition *parent;
  mtkx_peg_rule **rules;
  size_t count;
  size_t capacity;
  size_t next_order;
};

mtkx_peg_definition *mtkx_peg_definition_create(void) {
  return (mtkx_peg_definition *)mtkx_calloc(1, sizeof(mtkx_peg_definition));
}

mtkx_peg_definition *mtkx_peg_definition_derive(const mtkx_peg_definition *parent) {
  mtkx_peg_definition *d = mtkx_peg_definition_create();
  if (d != NULL) {
    d->parent = parent;
  }
  return d;
}

void mtkx_peg_definition_destroy(mtkx_peg_definition *definition) {
  size_t i;
  if (definition == NULL) {
    return;
  }
  for (i = 0; i < definition->count; ++i) {
    mtkx__peg_rule_free(definition->rules[i]);
  }
  mtkx_free(definition->rules);
  mtkx_free(definition);
}

const mtkx_peg_definition *mtkx_peg_definition_parent(const mtkx_peg_definition *d) {
  return d->parent;
}

mtkx_status mtkx_peg_definition_add_rule(mtkx_peg_definition *definition,
                                         const char *pattern, mtkx_peg_action_fn action,
                                         void *ctx, mtkx_peg_rule **out_rule) {
  mtkx_peg_rule *rule;
  const char *error;
  if (out_rule != NULL) {
    *out_rule = NULL;
  }
  if (definition == NULL || pattern == NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  if (mtkx_peg_pattern_error(pattern) != NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  rule = (mtkx_peg_rule *)mtkx_calloc(1, sizeof *rule);
  if (rule == NULL) {
    return MTKX_ERR_NOMEM;
  }
  rule->pattern_text = mtkx_strdup(pattern);
  rule->channel = mtkx_strdup(MTKX_PEG_DEFAULT_CHANNEL);
  error = mtkx__peg_compile(pattern, &rule->compiled);
  if (rule->pattern_text == NULL || rule->channel == NULL || error != NULL ||
      MTKX__GROW(definition->rules, definition->capacity, definition->count + 1) !=
          MTKX_OK) {
    mtkx__peg_rule_free(rule);
    return MTKX_ERR_NOMEM;
  }
  rule->semantic_action.fn = action;
  rule->semantic_action.ctx = ctx;
  rule->order = definition->next_order++;
  definition->rules[definition->count++] = rule;
  if (out_rule != NULL) {
    *out_rule = rule;
  }
  return MTKX_OK;
}

static size_t mtkx__peg_total(const mtkx_peg_definition *d) {
  size_t n = 0;
  for (; d != NULL; d = d->parent) {
    n += d->count;
  }
  return n;
}

/* Visible rule `index`, parent rules first. */
static const mtkx_peg_rule *mtkx__peg_rule_at(const mtkx_peg_definition *d, size_t index) {
  size_t inherited = d->parent != NULL ? mtkx__peg_total(d->parent) : 0;
  if (index < inherited) {
    return mtkx__peg_rule_at(d->parent, index);
  }
  return d->rules[index - inherited];
}

size_t mtkx_peg_definition_rules(const mtkx_peg_definition *definition,
                                 const mtkx_peg_rule **out, size_t capacity) {
  size_t total = mtkx__peg_total(definition);
  size_t i;
  if (out != NULL) {
    for (i = 0; i < total && i < capacity; ++i) {
      out[i] = mtkx__peg_rule_at(definition, i);
    }
  }
  return total;
}

static void mtkx__peg_fail(mtkx_peg_parse_result *result, const char *text, size_t length,
                           size_t pos, const char *message) {
  result->success = false;
  result->offset = pos;
  mtkx__line_col_at(text, length, pos, &result->line, &result->column);
  snprintf(result->message, sizeof result->message, "%s", message);
}

mtkx_peg_parse_result mtkx_peg_definition_parse(const mtkx_peg_definition *definition,
                                                const char *text, size_t length) {
  mtkx_peg_parse_result result;
  size_t total = mtkx__peg_total(definition);
  size_t pos = 0;
  memset(&result, 0, sizeof result);
  result.success = true;
  result.line = 1;
  result.column = 1;
  if (text == NULL) {
    text = "";
    length = 0;
  }

  while (pos < length) {
    bool matched = false;
    size_t i;
    for (i = 0; i < total; ++i) {
      const mtkx_peg_rule *r = mtkx__peg_rule_at(definition, i);
      size_t end;
      bool negated;
      if (!mtkx_peg_rule_try_match(r, text, length, pos, &end)) {
        continue;
      }
      negated = r->must_fail;
      if (!negated && end == pos) {
        /* a zero-width positive match would loop forever */
        mtkx__peg_fail(&result, text, length, pos,
                       "PEG rule matched empty input; refusing to loop");
        return result;
      }
      matched = true;
      if ((!negated || r->fire_on_neg) && r->semantic_action.fn != NULL) {
        mtkx_peg_match m = mtkx__make_match(text, length, pos, end, r);
        r->semantic_action.fn(&m, r->semantic_action.ctx);
      }
      if (r->if_succeed != NULL) {
        size_t end2;
        if (mtkx_peg_rule_try_match(r->if_succeed, text, length, pos, &end2) &&
            r->if_succeed->semantic_action.fn != NULL) {
          mtkx_peg_match m = mtkx__make_match(text, length, pos, end2, r->if_succeed);
          r->if_succeed->semantic_action.fn(&m, r->if_succeed->semantic_action.ctx);
        }
      }
      pos = end;
      break; /* ordered choice: the first success wins */
    }
    if (!matched) {
      char message[64];
      snprintf(message, sizeof message, "no PEG rule matched at offset %lu",
               (unsigned long)pos);
      mtkx__peg_fail(&result, text, length, pos, message);
      return result;
    }
  }
  result.offset = pos;
  return result;
}

/* ------------------------------------------------------------------ */
/*  Matcher                                                           */
/* ------------------------------------------------------------------ */

typedef enum mtkx__filter_mode { MTKX__FILTER_ALL, MTKX__FILTER_ONLY, MTKX__FILTER_EXCLUDE } mtkx__filter_mode;

struct mtkx_peg_matcher {
  const mtkx_peg_definition *def;
  const char *text;
  size_t length;
  size_t pos;
  mtkx__filter_mode mode;
  char **channels;
  size_t channel_count;
  size_t channel_capacity;
  mtkx_peg_arm *arms; /* channel strings owned */
  size_t arm_count;
  size_t arm_capacity;
};

mtkx_peg_arm mtkx_peg_rule_arm(const mtkx_peg_rule *rule, mtkx_peg_action_fn fn, void *ctx) {
  mtkx_peg_arm arm;
  memset(&arm, 0, sizeof arm);
  arm.rule = rule;
  arm.action.fn = fn;
  arm.action.ctx = ctx;
  return arm;
}

mtkx_peg_arm mtkx_peg_wildcard_arm(mtkx_peg_action_fn fn, void *ctx) {
  mtkx_peg_arm arm = mtkx_peg_rule_arm(NULL, fn, ctx);
  arm.is_wildcard = true;
  return arm;
}

mtkx_peg_arm mtkx_peg_channel_arm(const char *channel, mtkx_peg_action_fn fn, void *ctx) {
  mtkx_peg_arm arm = mtkx_peg_rule_arm(NULL, fn, ctx);
  arm.is_channel = true;
  arm.channel = channel;
  return arm;
}

mtkx_peg_matcher *mtkx_peg_matcher_create(const mtkx_peg_definition *definition,
                                          const char *text, size_t length) {
  mtkx_peg_matcher *m;
  if (definition == NULL) {
    return NULL;
  }
  m = (mtkx_peg_matcher *)mtkx_calloc(1, sizeof *m);
  if (m == NULL) {
    return NULL;
  }
  m->def = definition;
  m->text = text != NULL ? text : "";
  m->length = text != NULL ? length : 0;
  m->mode = MTKX__FILTER_ALL;
  return m;
}

static void mtkx__matcher_clear_arms(mtkx_peg_matcher *m) {
  size_t i;
  for (i = 0; i < m->arm_count; ++i) {
    mtkx_free((char *)m->arms[i].channel);
  }
  m->arm_count = 0;
}

static void mtkx__matcher_clear_channels(mtkx_peg_matcher *m) {
  size_t i;
  for (i = 0; i < m->channel_count; ++i) {
    mtkx_free(m->channels[i]);
  }
  m->channel_count = 0;
}

void mtkx_peg_matcher_destroy(mtkx_peg_matcher *matcher) {
  if (matcher == NULL) {
    return;
  }
  mtkx__matcher_clear_arms(matcher);
  mtkx__matcher_clear_channels(matcher);
  mtkx_free(matcher->arms);
  mtkx_free(matcher->channels);
  mtkx_free(matcher);
}

bool mtkx_peg_matcher_is_spent(const mtkx_peg_matcher *matcher) {
  return matcher->pos >= matcher->length;
}

size_t mtkx_peg_matcher_position(const mtkx_peg_matcher *matcher) { return matcher->pos; }

static mtkx_status mtkx__matcher_push_channel(mtkx_peg_matcher *m, const char *channel) {
  char *copy = mtkx_strdup(channel);
  if (copy == NULL ||
      MTKX__GROW(m->channels, m->channel_capacity, m->channel_count + 1) != MTKX_OK) {
    mtkx_free(copy);
    return MTKX_ERR_NOMEM;
  }
  m->channels[m->channel_count++] = copy;
  return MTKX_OK;
}

mtkx_status mtkx_peg_matcher_only(mtkx_peg_matcher *matcher, const char *channel) {
  matcher->mode = MTKX__FILTER_ONLY;
  mtkx__matcher_clear_channels(matcher);
  return mtkx__matcher_push_channel(matcher, channel != NULL ? channel : "");
}

mtkx_status mtkx_peg_matcher_include(mtkx_peg_matcher *matcher, const char *channel) {
  /* As in the C++ matcher: include() switches an exclude filter to an
   * "only" filter, but leaves the unfiltered (all) mode unchanged. */
  if (matcher->mode == MTKX__FILTER_EXCLUDE) {
    matcher->mode = MTKX__FILTER_ONLY;
  }
  return mtkx__matcher_push_channel(matcher, channel != NULL ? channel : "");
}

mtkx_status mtkx_peg_matcher_exclude(mtkx_peg_matcher *matcher, const char *channel) {
  if (matcher->mode != MTKX__FILTER_EXCLUDE) {
    matcher->mode = MTKX__FILTER_EXCLUDE;
    mtkx__matcher_clear_channels(matcher);
  }
  return mtkx__matcher_push_channel(matcher, channel != NULL ? channel : "");
}

bool mtkx_peg_matcher_channel_allowed(const mtkx_peg_matcher *matcher, const char *channel) {
  size_t i;
  bool found = false;
  if (matcher->mode == MTKX__FILTER_ALL) {
    return true;
  }
  for (i = 0; i < matcher->channel_count; ++i) {
    if (strcmp(matcher->channels[i], channel != NULL ? channel : "") == 0) {
      found = true;
      break;
    }
  }
  return matcher->mode == MTKX__FILTER_ONLY ? found : !found;
}

static void mtkx__matcher_skip_ignored(mtkx_peg_matcher *m) {
  bool progressed = true;
  size_t total = mtkx__peg_total(m->def);
  while (progressed && !mtkx_peg_matcher_is_spent(m)) {
    size_t i;
    progressed = false;
    for (i = 0; i < total; ++i) {
      const mtkx_peg_rule *r = mtkx__peg_rule_at(m->def, i);
      size_t end;
      if (!mtkx_peg_channel_is_ignore(mtkx__rule_channel(r))) {
        continue;
      }
      if (mtkx_peg_rule_try_match(r, m->text, m->length, m->pos, &end)) {
        if (end == m->pos) {
          continue;
        }
        m->pos = end;
        progressed = true;
        break;
      }
    }
  }
}

static void mtkx__matcher_fire(mtkx_peg_matcher *m, mtkx_peg_action action,
                               const mtkx_peg_rule *rule, size_t begin, size_t end) {
  mtkx_peg_match match = mtkx__make_match(m->text, m->length, begin, end, rule);
  m->pos = end;
  if (action.fn != NULL) {
    action.fn(&match, action.ctx);
  } else if (rule->semantic_action.fn != NULL) {
    rule->semantic_action.fn(&match, rule->semantic_action.ctx);
  }
}

static void mtkx__matcher_step(mtkx_peg_matcher *m) {
  size_t i;
  size_t total;
  if (m->arm_count == 0 || mtkx_peg_matcher_is_spent(m)) {
    return;
  }
  mtkx__matcher_skip_ignored(m);
  if (mtkx_peg_matcher_is_spent(m)) {
    mtkx__matcher_clear_arms(m);
    return;
  }
  total = mtkx__peg_total(m->def);
  /* Non-wildcard arms in order: the first match wins. Actions may feed the
   * matcher re-entrantly, so the arm is copied and arms are cleared before
   * firing. */
  for (i = 0; i < m->arm_count; ++i) {
    const mtkx_peg_arm *arm = &m->arms[i];
    size_t end;
    if (arm->is_wildcard) {
      continue;
    }
    if (arm->is_channel) {
      size_t k;
      for (k = 0; k < total; ++k) {
        const mtkx_peg_rule *r = mtkx__peg_rule_at(m->def, k);
        if (strcmp(mtkx__rule_channel(r), arm->channel != NULL ? arm->channel : "") != 0) {
          continue;
        }
        if (mtkx_peg_rule_try_match(r, m->text, m->length, m->pos, &end)) {
          mtkx_peg_action action = arm->action;
          size_t begin = m->pos;
          mtkx__matcher_clear_arms(m);
          mtkx__matcher_fire(m, action, r, begin, end);
          return;
        }
      }
      continue;
    }
    if (arm->rule == NULL ||
        !mtkx_peg_matcher_channel_allowed(m, mtkx__rule_channel(arm->rule))) {
      continue;
    }
    if (mtkx_peg_rule_try_match(arm->rule, m->text, m->length, m->pos, &end)) {
      mtkx_peg_action action = arm->action;
      const mtkx_peg_rule *rule = arm->rule;
      size_t begin = m->pos;
      mtkx__matcher_clear_arms(m);
      mtkx__matcher_fire(m, action, rule, begin, end);
      return;
    }
  }
  /* No rule arm matched: fire the first wildcard arm, if any. */
  for (i = 0; i < m->arm_count; ++i) {
    if (m->arms[i].is_wildcard) {
      mtkx_peg_action action = m->arms[i].action;
      mtkx_peg_match match = mtkx__make_match(m->text, m->length, m->pos, m->pos + 1, NULL);
      mtkx__matcher_clear_arms(m);
      ++m->pos; /* the wildcard consumes one character */
      if (action.fn != NULL) {
        action.fn(&match, action.ctx);
      }
      return;
    }
  }
  /* Nothing fired: keep the arms; a later feed may match here. */
}

mtkx_status mtkx_peg_matcher_feed(mtkx_peg_matcher *matcher, mtkx_peg_arm arm) {
  if (arm.is_channel) {
    arm.channel = mtkx_strdup(arm.channel != NULL ? arm.channel : "");
    if (arm.channel == NULL) {
      return MTKX_ERR_NOMEM;
    }
  } else {
    arm.channel = NULL;
  }
  if (MTKX__GROW(matcher->arms, matcher->arm_capacity, matcher->arm_count + 1) != MTKX_OK) {
    mtkx_free((char *)arm.channel);
    return MTKX_ERR_NOMEM;
  }
  matcher->arms[matcher->arm_count++] = arm;
  mtkx__matcher_step(matcher);
  return MTKX_OK;
}

size_t mtkx_peg_matcher_pending_arms(const mtkx_peg_matcher *matcher) {
  return matcher->arm_count;
}

/* ------------------------------------------------------------------ */
/*  PEG combinators                                                   */
/* ------------------------------------------------------------------ */

typedef struct mtkx__peg_ctx {
  mtkx_parser *a;
  mtkx_parser *b;
  const mtkx_peg_rule *rule;
  char *channel;
} mtkx__peg_ctx;

static void mtkx__peg_ctx_free(void *ctx) {
  mtkx__peg_ctx *c = (mtkx__peg_ctx *)ctx;
  mtkx_parser_release(c->a);
  mtkx_parser_release(c->b);
  mtkx_free(c->channel);
  mtkx_free(c);
}

static mtkx_parser *mtkx__peg_new(mtkx_parse_fn fn, mtkx_parser *a, mtkx_parser *b,
                                  int arity, const mtkx_peg_rule *rule,
                                  const char *channel) {
  mtkx__peg_ctx *c;
  if ((arity >= 1 && a == NULL) || (arity == 2 && b == NULL)) {
    mtkx_parser_release(a);
    mtkx_parser_release(b);
    return NULL;
  }
  c = (mtkx__peg_ctx *)mtkx_calloc(1, sizeof *c);
  if (c == NULL) {
    mtkx_parser_release(a);
    mtkx_parser_release(b);
    return NULL;
  }
  c->a = a;
  c->b = b;
  c->rule = rule;
  if (channel != NULL && (c->channel = mtkx_strdup(channel)) == NULL) {
    mtkx__peg_ctx_free(c);
    return NULL;
  }
  return mtkx_parser_new(fn, c, mtkx__peg_ctx_free);
}

static mtkx_peg_match mtkx__span_match(const mtkx_parse_input *in, size_t begin) {
  mtkx_peg_match m;
  memset(&m, 0, sizeof m);
  m.begin = begin;
  m.end = in->pos;
  m.value = mtkx_parse_input_span(in, begin);
  m.channel = mtkx__default_channel;
  m.line = 1;
  m.column = 1;
  return m;
}

static mtkx_parse_result mtkx__as_span_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__peg_ctx *c = (const mtkx__peg_ctx *)ctx;
  size_t begin = in->pos;
  mtkx_parse_result r = mtkx_parser_parse(c->a, in);
  mtkx_peg_match m;
  if (!r.ok) {
    return r;
  }
  mtkx_parse_result_free(&r);
  m = mtkx__span_match(in, begin);
  return mtkx_parse_ok(mtkx_parse_value_match(&m));
}

mtkx_parser *mtkx_peg_as_span(mtkx_parser *p) {
  return mtkx__peg_new(mtkx__as_span_parse, p, NULL, 1, NULL, NULL);
}

static mtkx_parse_result mtkx__peg_seq2_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__peg_ctx *c = (const mtkx__peg_ctx *)ctx;
  size_t begin = in->pos;
  mtkx_parse_result ra = mtkx_parser_parse(c->a, in);
  mtkx_parse_result rb;
  mtkx_peg_match m;
  if (!ra.ok) {
    return ra;
  }
  mtkx_parse_result_free(&ra);
  rb = mtkx_parser_parse(c->b, in);
  if (!rb.ok) {
    bool consumed = in->pos != begin;
    in->pos = begin;
    if (consumed) {
      rb.error.kind = MTKX_PARSE_COMMITTED;
    }
    return rb;
  }
  mtkx_parse_result_free(&rb);
  m = mtkx__span_match(in, begin);
  return mtkx_parse_ok(mtkx_parse_value_match(&m));
}

static mtkx_parser *mtkx__release_va(size_t from, size_t count, va_list args) {
  size_t i;
  for (i = from; i < count; ++i) {
    mtkx_parser_release(va_arg(args, mtkx_parser *));
  }
  return NULL;
}

mtkx_parser *mtkx_peg_seq(size_t count, ...) {
  va_list args;
  mtkx_parser *acc;
  size_t i;
  if (count == 0) {
    return NULL;
  }
  va_start(args, count);
  acc = va_arg(args, mtkx_parser *);
  if (count == 1) {
    acc = mtkx_peg_as_span(acc);
  }
  for (i = 1; i < count; ++i) {
    mtkx_parser *next = va_arg(args, mtkx_parser *);
    acc = mtkx__peg_new(mtkx__peg_seq2_parse, acc, next, 2, NULL, NULL);
    if (acc == NULL) {
      mtkx__release_va(i + 1, count, args);
      break;
    }
  }
  va_end(args);
  return acc;
}

mtkx_parser *mtkx_peg_choice(size_t count, ...) {
  va_list args;
  mtkx_parser *acc;
  size_t i;
  if (count == 0) {
    return NULL;
  }
  va_start(args, count);
  acc = va_arg(args, mtkx_parser *);
  for (i = 1; i < count; ++i) {
    mtkx_parser *next = va_arg(args, mtkx_parser *);
    acc = mtkx_parser_alt(mtkx_parser_try(acc), mtkx_parser_try(next));
    if (acc == NULL) {
      mtkx__release_va(i + 1, count, args);
      break;
    }
  }
  va_end(args);
  return acc;
}

mtkx_parser *mtkx_peg_opt(mtkx_parser *p) { return mtkx_parser_optional(mtkx_parser_try(p)); }

mtkx_parser *mtkx_peg_many(mtkx_parser *p) { return mtkx_parser_many(mtkx_parser_try(p)); }

mtkx_parser *mtkx_peg_many1(mtkx_parser *p) {
  mtkx_parser *again = mtkx_parser_retain(p);
  return mtkx_parser_seq(p, mtkx_parser_many(mtkx_parser_try(again)));
}

static mtkx_parse_result mtkx__peg_and_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__peg_ctx *c = (const mtkx__peg_ctx *)ctx;
  size_t save = in->pos;
  mtkx_parse_result r = mtkx_parser_parse(c->a, in);
  in->pos = save;
  if (!r.ok) {
    if (!r.error.out_of_memory) {
      r.error.kind = MTKX_PARSE_SOFT;
    }
    return r;
  }
  mtkx_parse_result_free(&r);
  return mtkx_parse_ok(mtkx_parse_value_bool(true));
}

mtkx_parser *mtkx_peg_and(mtkx_parser *p) {
  return mtkx__peg_new(mtkx__peg_and_parse, p, NULL, 1, NULL, NULL);
}

static mtkx_parse_result mtkx__peg_not_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__peg_ctx *c = (const mtkx__peg_ctx *)ctx;
  size_t save = in->pos;
  mtkx_parse_result r = mtkx_parser_parse(c->a, in);
  in->pos = save;
  if (r.ok) {
    mtkx_parse_result_free(&r);
    return mtkx_parse_fail_expected(in, "negative lookahead", MTKX_PARSE_SOFT);
  }
  if (r.error.out_of_memory) {
    return r;
  }
  mtkx_parse_result_free(&r);
  return mtkx_parse_ok(mtkx_parse_value_bool(true));
}

mtkx_parser *mtkx_peg_not(mtkx_parser *p) {
  return mtkx__peg_new(mtkx__peg_not_parse, p, NULL, 1, NULL, NULL);
}

static mtkx_parse_result mtkx__peg_rule_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__peg_ctx *c = (const mtkx__peg_ctx *)ctx;
  size_t begin = in->pos;
  size_t end;
  mtkx_peg_match m;
  if (!mtkx_peg_rule_try_match(c->rule, in->source, in->length, begin, &end)) {
    return mtkx_parse_fail_expected(in, c->rule->pattern_text, MTKX_PARSE_SOFT);
  }
  in->pos = end;
  m = mtkx__make_match(in->source, in->length, begin, end, c->rule);
  return mtkx_parse_ok(mtkx_parse_value_match(&m));
}

mtkx_parser *mtkx_peg_rule_parser(const mtkx_peg_rule *rule) {
  if (rule == NULL) {
    return NULL;
  }
  return mtkx__peg_new(mtkx__peg_rule_parse, NULL, NULL, 0, rule, NULL);
}

static mtkx_parse_result mtkx__peg_channel_parse(mtkx_parse_input *in, void *ctx) {
  const mtkx__peg_ctx *c = (const mtkx__peg_ctx *)ctx;
  size_t begin = in->pos;
  mtkx_parse_result r = mtkx_parser_parse(c->a, in);
  mtkx_peg_match m;
  if (!r.ok) {
    return r;
  }
  mtkx_parse_result_free(&r);
  m = mtkx__make_match(in->source, in->length, begin, in->pos, NULL);
  m.channel = c->channel;
  m.ignored = mtkx_peg_channel_is_ignore(c->channel);
  return mtkx_parse_ok(mtkx_parse_value_match(&m));
}

mtkx_parser *mtkx_peg_channel(const char *channel, mtkx_parser *p) {
  if (mtkx_peg_channel_validate(channel) != MTKX_OK) {
    mtkx_parser_release(p);
    return NULL;
  }
  return mtkx__peg_new(mtkx__peg_channel_parse, p, NULL, 1, NULL, channel);
}
