/**
 * @file memtkx/dsl/ast.h
 * @brief Tagged AST nodes with S-expression dumps, and fixpoint rewrite
 *        rules (C++ `leaf<"tag">`, `node<"tag">`, `rule<"name">`,
 *        `rewrite_set`).
 *
 * Nodes are heap objects. Constructors that take children take ownership
 * of them, and accept NULL children (from a failed allocation) by freeing
 * the rest and returning NULL, so nested construction needs a single
 * NULL check:
 *
 * @code
 *   mtkx_ast_node *expr = mtkx_ast_node_of("add", 2,
 *       mtkx_ast_leaf_int("num", 1), mtkx_ast_leaf_int("num", 2));
 *   if (expr != NULL) {
 *     char *text = mtkx_ast_dump(expr);   // "(add (num 1) (num 2))"
 *     mtkx_free(text);
 *     mtkx_ast_free(expr);
 *   }
 * @endcode
 */
#ifndef MEMTKX_DSL_AST_H
#define MEMTKX_DSL_AST_H

#include <stdarg.h>
#include <stddef.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/** An AST node (opaque). Leaves hold a string value; inner nodes hold children. */
typedef struct mtkx_ast_node mtkx_ast_node;

/** Creates an empty leaf with tag "" (C++ default `ASTNode`). */
MTKX_API mtkx_ast_node *mtkx_ast_empty(void);

/** Creates a leaf with a copy of @p tag and @p value (C++ `leaf<Tag>(str)`). */
MTKX_API mtkx_ast_node *mtkx_ast_leaf(const char *tag, const char *value);

/** Creates a leaf whose value is the decimal form of @p value. */
MTKX_API mtkx_ast_node *mtkx_ast_leaf_int(const char *tag, long long value);

/** Creates a leaf whose value is the decimal form of @p value. */
MTKX_API mtkx_ast_node *mtkx_ast_leaf_uint(const char *tag, unsigned long long value);

/** Creates a leaf whose value is @p value formatted like an ostream ("%g"). */
MTKX_API mtkx_ast_node *mtkx_ast_leaf_double(const char *tag, double value);

/** Creates a leaf whose value is produced by printf-style formatting. */
MTKX_API mtkx_ast_node *mtkx_ast_leaf_fmt(const char *tag, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/**
 * @brief Creates an inner node owning @p count children (C++ `node<Tag>(...)`).
 *
 * Ownership of every child transfers to the call, even on failure. If any
 * child is NULL, or allocation fails, all children are freed and NULL is
 * returned.
 */
MTKX_API mtkx_ast_node *mtkx_ast_node_new(const char *tag, size_t count,
                                          mtkx_ast_node *const *children);

/** Variadic form of mtkx_ast_node_new(): @p count `mtkx_ast_node *` arguments. */
MTKX_API mtkx_ast_node *mtkx_ast_node_of(const char *tag, size_t count, ...);

/** va_list form of mtkx_ast_node_of(). */
MTKX_API mtkx_ast_node *mtkx_ast_node_ofv(const char *tag, size_t count, va_list args);

/** Releases a node and all of its descendants (NULL is ignored). */
MTKX_API void mtkx_ast_free(mtkx_ast_node *node);

/** @return a deep copy of @p node, or NULL on allocation failure. */
MTKX_API mtkx_ast_node *mtkx_ast_clone(const mtkx_ast_node *node);

/** @return the node's tag (never NULL). */
MTKX_API const char *mtkx_ast_tag(const mtkx_ast_node *node);

/** @return true when the node's tag equals @p tag. */
MTKX_API bool mtkx_ast_tag_is(const mtkx_ast_node *node, const char *tag);

/** @return true for leaves. */
MTKX_API bool mtkx_ast_is_leaf(const mtkx_ast_node *node);

/** @return the leaf value ("" for inner nodes; never NULL). */
MTKX_API const char *mtkx_ast_value(const mtkx_ast_node *node);

/** @return the number of children (0 for leaves). */
MTKX_API size_t mtkx_ast_size(const mtkx_ast_node *node);

/** @return child @p index, or NULL when out of range. */
MTKX_API mtkx_ast_node *mtkx_ast_child(const mtkx_ast_node *node, size_t index);

/**
 * @brief Appends @p child (ownership transfers) to an inner node.
 * @return MTKX_ERR_INVALID_ARGUMENT for leaves or a NULL child (the child
 *         is freed), MTKX_ERR_NOMEM on allocation failure (child freed).
 */
MTKX_API mtkx_status mtkx_ast_append_child(mtkx_ast_node *node, mtkx_ast_node *child);

/**
 * @brief Removes child @p index from @p node and returns it to the caller
 *        (NULL when out of range). Useful in rewrite transformers.
 */
MTKX_API mtkx_ast_node *mtkx_ast_take_child(mtkx_ast_node *node, size_t index);

/** Structural equality: tags, leaf-ness, values and children recursively. */
MTKX_API bool mtkx_ast_equal(const mtkx_ast_node *a, const mtkx_ast_node *b);

/**
 * @brief Renders `(tag value)` for leaves and `(tag child ...)` for inner
 *        nodes. Release the string with mtkx_free(). NULL on OOM.
 */
MTKX_API char *mtkx_ast_dump(const mtkx_ast_node *node);

/* ------------------------------------------------------------------ */
/*  Rewrite rules                                                     */
/* ------------------------------------------------------------------ */

/** Decides whether a rule applies to @p node. */
typedef bool (*mtkx_rewrite_pred_fn)(const mtkx_ast_node *node, void *ctx);

/**
 * @brief Produces the replacement for @p node as a new tree owned by the
 *        caller (clone parts of @p node with mtkx_ast_clone()). Returning
 *        NULL reports allocation failure and aborts the rewrite.
 */
typedef mtkx_ast_node *(*mtkx_rewrite_trans_fn)(const mtkx_ast_node *node, void *ctx);

/** A named rule (C++ `RewriteRule`). */
typedef struct mtkx_rewrite_rule {
  const char *name;
  mtkx_rewrite_pred_fn pred;
  mtkx_rewrite_trans_fn trans;
  void *ctx;
} mtkx_rewrite_rule;

/** Builds a rule (C++ `rule<"name">(pred, trans)`). */
MTKX_INLINE mtkx_rewrite_rule mtkx_rewrite_rule_make(const char *name,
                                                     mtkx_rewrite_pred_fn pred,
                                                     mtkx_rewrite_trans_fn trans,
                                                     void *ctx) {
  mtkx_rewrite_rule r;
  r.name = name;
  r.pred = pred;
  r.trans = trans;
  r.ctx = ctx;
  return r;
}

/** Default iteration bound of a rewrite set. */
#define MTKX_REWRITE_DEFAULT_MAX_ITERATIONS 100u

/** An ordered set of caller-owned rules (C++ `RewriteSet`). */
typedef struct mtkx_rewrite_set {
  const mtkx_rewrite_rule *rules;
  size_t count;
  size_t max_iterations;
} mtkx_rewrite_set;

/** Builds a set with max_iterations = MTKX_REWRITE_DEFAULT_MAX_ITERATIONS. */
MTKX_INLINE mtkx_rewrite_set mtkx_rewrite_set_make(const mtkx_rewrite_rule *rules,
                                                   size_t count) {
  mtkx_rewrite_set s;
  s.rules = rules;
  s.count = count;
  s.max_iterations = MTKX_REWRITE_DEFAULT_MAX_ITERATIONS;
  return s;
}

/**
 * @brief Rewrites a copy of @p root bottom-up until no rule fires or
 *        `max_iterations` passes have run.
 *
 * Within one pass, children are rewritten first; then every rule is tried
 * in order on the resulting node, each one seeing the previous rule's
 * output. @p root is not modified.
 *
 * @return the new tree (free with mtkx_ast_free()), or NULL on failure.
 */
MTKX_API mtkx_ast_node *mtkx_rewrite_apply(const mtkx_rewrite_set *set,
                                           const mtkx_ast_node *root);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_AST_H */
