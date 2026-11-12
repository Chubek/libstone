/**
 * @file memtkx/dsl/features.h
 * @brief Feature flags: the C counterpart of `DSL<Derived, Features...>`
 *        composition and the `HasFeature` / `Pipeable` / ... concepts.
 *
 * C has no CRTP mixins. In C, the behaviour each C++ feature mixed in is
 * an ordinary function family (pipelines, predicates, match tables ...),
 * so a "DSL" is any struct of your own. When it is useful to describe
 * which families a component supports (for registries, assertions or
 * documentation), store a bitmask of these flags in it.
 *
 * @code
 *   struct my_dsl { unsigned features; ... };
 *   struct my_dsl d = { MTKX_FEATURE_PIPELINE | MTKX_FEATURE_OPERATORS };
 *   if (mtkx_has_feature(d.features, MTKX_FEATURE_PIPELINE)) ...
 * @endcode
 */
#ifndef MEMTKX_DSL_FEATURES_H
#define MEMTKX_DSL_FEATURES_H

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/** One bit per C++ feature tag. */
typedef enum mtkx_feature {
  MTKX_FEATURE_PIPELINE = 1u << 0,          /**< dsl/pipeline.h: mtkx_pipe */
  MTKX_FEATURE_OPERATORS = 1u << 1,         /**< dsl/pipeline.h: mtkx_predicate */
  MTKX_FEATURE_PATTERN_MATCH = 1u << 2,     /**< dsl/match.h */
  MTKX_FEATURE_AST = 1u << 3,               /**< dsl/ast.h nodes */
  MTKX_FEATURE_REWRITE = 1u << 4,           /**< dsl/ast.h rewrite sets */
  MTKX_FEATURE_EXPR_TEMPLATES = 1u << 5,    /**< dsl/expr.h */
  MTKX_FEATURE_CUSTOM_LITERALS = 1u << 6,   /**< dsl/literals.h */
  MTKX_FEATURE_MEMOIZATION = 1u << 7,       /**< dsl/memo.h mtkx_memo */
  MTKX_FEATURE_LAZY = 1u << 8,              /**< dsl/memo.h mtkx_lazy */
  MTKX_FEATURE_MONADIC = 1u << 9,           /**< dsl/result.h MTKX_DEFINE_MAYBE */
  MTKX_FEATURE_RESULT = 1u << 10,           /**< dsl/result.h MTKX_DEFINE_RESULT */
  MTKX_FEATURE_COMBINATOR_PARSER = 1u << 11,/**< dsl/parser.h, dsl/peg.h */
  MTKX_FEATURE_TASK_PIPELINE = 1u << 12     /**< dsl/task.h */
} mtkx_feature;

/** @return true when every bit of @p feature is set in @p features. */
MTKX_INLINE bool mtkx_has_feature(unsigned features, unsigned feature) {
  return (features & feature) == feature;
}

/** @return a static name such as "Pipeline" for a single feature bit. */
MTKX_API const char *mtkx_feature_name(mtkx_feature feature);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_FEATURES_H */
