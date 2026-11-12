#include "memtkx/dsl/ast.h"

#include <stdio.h>
#include <string.h>

#include "internal/common.h"
#include "internal/strbuf.h"

struct mtkx_ast_node {
  char *tag;
  char *value;
  mtkx_ast_node **children;
  size_t count;
  size_t capacity;
  bool is_leaf;
};

static mtkx_ast_node *mtkx__ast_alloc(const char *tag, bool is_leaf) {
  mtkx_ast_node *node = (mtkx_ast_node *)mtkx_calloc(1, sizeof *node);
  if (node == NULL) {
    return NULL;
  }
  node->tag = mtkx_strdup(tag != NULL ? tag : "");
  node->is_leaf = is_leaf;
  if (node->tag == NULL) {
    mtkx_free(node);
    return NULL;
  }
  return node;
}

mtkx_ast_node *mtkx_ast_empty(void) { return mtkx_ast_leaf("", ""); }

mtkx_ast_node *mtkx_ast_leaf(const char *tag, const char *value) {
  mtkx_ast_node *node = mtkx__ast_alloc(tag, true);
  if (node == NULL) {
    return NULL;
  }
  node->value = mtkx_strdup(value != NULL ? value : "");
  if (node->value == NULL) {
    mtkx_ast_free(node);
    return NULL;
  }
  return node;
}

static mtkx_ast_node *mtkx__ast_leaf_vfmt(const char *tag, const char *format,
                                          va_list args) {
  mtkx__strbuf sb;
  char *text;
  mtkx_ast_node *node;
  mtkx__strbuf_init(&sb);
  mtkx__strbuf_vappendf(&sb, format, args);
  text = mtkx__strbuf_finish(&sb);
  if (text == NULL) {
    return NULL;
  }
  node = mtkx__ast_alloc(tag, true);
  if (node == NULL) {
    mtkx_free(text);
    return NULL;
  }
  node->value = text;
  return node;
}

mtkx_ast_node *mtkx_ast_leaf_fmt(const char *tag, const char *format, ...) {
  va_list args;
  mtkx_ast_node *node;
  va_start(args, format);
  node = mtkx__ast_leaf_vfmt(tag, format, args);
  va_end(args);
  return node;
}

mtkx_ast_node *mtkx_ast_leaf_int(const char *tag, long long value) {
  return mtkx_ast_leaf_fmt(tag, "%lld", value);
}

mtkx_ast_node *mtkx_ast_leaf_uint(const char *tag, unsigned long long value) {
  return mtkx_ast_leaf_fmt(tag, "%llu", value);
}

mtkx_ast_node *mtkx_ast_leaf_double(const char *tag, double value) {
  return mtkx_ast_leaf_fmt(tag, "%g", value);
}

mtkx_ast_node *mtkx_ast_node_new(const char *tag, size_t count,
                                 mtkx_ast_node *const *children) {
  mtkx_ast_node *node = NULL;
  size_t i;
  bool ok = true;
  for (i = 0; i < count; ++i) {
    if (children[i] == NULL) {
      ok = false;
    }
  }
  if (ok) {
    node = mtkx__ast_alloc(tag, false);
    if (node != NULL && count != 0) {
      if (MTKX__GROW(node->children, node->capacity, count) != MTKX_OK) {
        mtkx_ast_free(node);
        node = NULL;
      }
    }
  }
  if (node == NULL) {
    for (i = 0; i < count; ++i) {
      mtkx_ast_free(children[i]);
    }
    return NULL;
  }
  for (i = 0; i < count; ++i) {
    node->children[i] = children[i];
  }
  node->count = count;
  return node;
}

mtkx_ast_node *mtkx_ast_node_ofv(const char *tag, size_t count, va_list args) {
  mtkx_ast_node *local[16] = {NULL};
  mtkx_ast_node **children = local;
  mtkx_ast_node *node;
  size_t i;
  bool heap = count > sizeof local / sizeof local[0];
  if (heap) {
    size_t bytes;
    if (!mtkx__mul_size(count, sizeof *children, &bytes) ||
        (children = (mtkx_ast_node **)mtkx_malloc(bytes)) == NULL) {
      for (i = 0; i < count; ++i) {
        mtkx_ast_free(va_arg(args, mtkx_ast_node *));
      }
      return NULL;
    }
  }
  for (i = 0; i < count; ++i) {
    children[i] = va_arg(args, mtkx_ast_node *);
  }
  node = mtkx_ast_node_new(tag, count, children);
  if (heap) {
    mtkx_free(children);
  }
  return node;
}

mtkx_ast_node *mtkx_ast_node_of(const char *tag, size_t count, ...) {
  va_list args;
  mtkx_ast_node *node;
  va_start(args, count);
  node = mtkx_ast_node_ofv(tag, count, args);
  va_end(args);
  return node;
}

void mtkx_ast_free(mtkx_ast_node *node) {
  size_t i;
  if (node == NULL) {
    return;
  }
  for (i = 0; i < node->count; ++i) {
    mtkx_ast_free(node->children[i]);
  }
  mtkx_free(node->children);
  mtkx_free(node->tag);
  mtkx_free(node->value);
  mtkx_free(node);
}

mtkx_ast_node *mtkx_ast_clone(const mtkx_ast_node *node) {
  mtkx_ast_node *copy;
  size_t i;
  if (node == NULL) {
    return NULL;
  }
  if (node->is_leaf) {
    copy = mtkx_ast_leaf(node->tag, node->value);
    /* a leaf with appended children is impossible via the API */
    return copy;
  }
  copy = mtkx__ast_alloc(node->tag, false);
  if (copy == NULL) {
    return NULL;
  }
  if (node->count != 0 &&
      MTKX__GROW(copy->children, copy->capacity, node->count) != MTKX_OK) {
    mtkx_ast_free(copy);
    return NULL;
  }
  for (i = 0; i < node->count; ++i) {
    mtkx_ast_node *child = mtkx_ast_clone(node->children[i]);
    if (child == NULL) {
      mtkx_ast_free(copy);
      return NULL;
    }
    copy->children[copy->count++] = child;
  }
  return copy;
}

const char *mtkx_ast_tag(const mtkx_ast_node *node) { return node->tag; }

bool mtkx_ast_tag_is(const mtkx_ast_node *node, const char *tag) {
  return node != NULL && tag != NULL && strcmp(node->tag, tag) == 0;
}

bool mtkx_ast_is_leaf(const mtkx_ast_node *node) { return node->is_leaf; }

const char *mtkx_ast_value(const mtkx_ast_node *node) {
  return node->value != NULL ? node->value : "";
}

size_t mtkx_ast_size(const mtkx_ast_node *node) { return node->count; }

mtkx_ast_node *mtkx_ast_child(const mtkx_ast_node *node, size_t index) {
  return (node != NULL && index < node->count) ? node->children[index] : NULL;
}

mtkx_status mtkx_ast_append_child(mtkx_ast_node *node, mtkx_ast_node *child) {
  if (node == NULL || child == NULL || node->is_leaf) {
    mtkx_ast_free(child);
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  if (MTKX__GROW(node->children, node->capacity, node->count + 1) != MTKX_OK) {
    mtkx_ast_free(child);
    return MTKX_ERR_NOMEM;
  }
  node->children[node->count++] = child;
  return MTKX_OK;
}

mtkx_ast_node *mtkx_ast_take_child(mtkx_ast_node *node, size_t index) {
  mtkx_ast_node *child;
  if (node == NULL || index >= node->count) {
    return NULL;
  }
  child = node->children[index];
  memmove(node->children + index, node->children + index + 1,
          (node->count - index - 1) * sizeof *node->children);
  --node->count;
  return child;
}

bool mtkx_ast_equal(const mtkx_ast_node *a, const mtkx_ast_node *b) {
  size_t i;
  if (a == b) {
    return true;
  }
  if (a == NULL || b == NULL) {
    return false;
  }
  if (strcmp(a->tag, b->tag) != 0 || a->is_leaf != b->is_leaf) {
    return false;
  }
  if (a->is_leaf) {
    return strcmp(mtkx_ast_value(a), mtkx_ast_value(b)) == 0;
  }
  if (a->count != b->count) {
    return false;
  }
  for (i = 0; i < a->count; ++i) {
    if (!mtkx_ast_equal(a->children[i], b->children[i])) {
      return false;
    }
  }
  return true;
}

static void mtkx__ast_dump(const mtkx_ast_node *node, mtkx__strbuf *sb) {
  size_t i;
  mtkx__strbuf_append_char(sb, '(');
  mtkx__strbuf_append_cstr(sb, node->tag);
  if (node->is_leaf) {
    if (node->value != NULL && node->value[0] != '\0') {
      mtkx__strbuf_append_char(sb, ' ');
      mtkx__strbuf_append_cstr(sb, node->value);
    }
  } else {
    for (i = 0; i < node->count; ++i) {
      mtkx__strbuf_append_char(sb, ' ');
      mtkx__ast_dump(node->children[i], sb);
    }
  }
  mtkx__strbuf_append_char(sb, ')');
}

char *mtkx_ast_dump(const mtkx_ast_node *node) {
  mtkx__strbuf sb;
  if (node == NULL) {
    return NULL;
  }
  mtkx__strbuf_init(&sb);
  mtkx__ast_dump(node, &sb);
  return mtkx__strbuf_finish(&sb);
}

/* ------------------------------------------------------------------ */
/*  Rewrite                                                           */
/* ------------------------------------------------------------------ */

/*
 * One bottom-up pass over `node` (port of RewriteSet::rewrite_once).
 * Consumes `node` and returns its replacement, or NULL on failure.
 */
static mtkx_ast_node *mtkx__rewrite_once(const mtkx_rewrite_set *set, mtkx_ast_node *node,
                                         bool *changed) {
  size_t i;
  if (!node->is_leaf) {
    for (i = 0; i < node->count; ++i) {
      mtkx_ast_node *rewritten = mtkx__rewrite_once(set, node->children[i], changed);
      node->children[i] = rewritten;
      if (rewritten == NULL) {
        mtkx_ast_free(node);
        return NULL;
      }
    }
  }
  for (i = 0; i < set->count; ++i) {
    const mtkx_rewrite_rule *rule = &set->rules[i];
    if (rule->pred != NULL && rule->trans != NULL && rule->pred(node, rule->ctx)) {
      mtkx_ast_node *replacement = rule->trans(node, rule->ctx);
      mtkx_ast_free(node);
      if (replacement == NULL) {
        return NULL;
      }
      node = replacement;
      *changed = true;
    }
  }
  return node;
}

mtkx_ast_node *mtkx_rewrite_apply(const mtkx_rewrite_set *set, const mtkx_ast_node *root) {
  mtkx_ast_node *current;
  size_t iteration;
  if (set == NULL || root == NULL) {
    return NULL;
  }
  current = mtkx_ast_clone(root);
  if (current == NULL) {
    return NULL;
  }
  for (iteration = 0; iteration < set->max_iterations; ++iteration) {
    bool changed = false;
    current = mtkx__rewrite_once(set, current, &changed);
    if (current == NULL || !changed) {
      return current;
    }
  }
  return current;
}
