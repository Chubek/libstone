/**
 * @file memtkx/dsl/pipeline.h
 * @brief Pipelines (C++ `value | pipe(f1) | pipe(f2)`) and composable
 *        predicates (C++ `pred_a & pred_b`, `|`, `!`).
 *
 * A pipeline stage transforms a value in place and may short-circuit the
 * rest of the pipeline by returning false (the C equivalent of a stage
 * returning `std::nullopt`). The value is a `void *` to caller storage;
 * when stages change the value's "type", use a struct or union that holds
 * every representation.
 *
 * @code
 *   static bool times_two(void *v, void *ctx) { *(int *)v *= 2; return true; }
 *   static bool plus_one(void *v, void *ctx)  { *(int *)v += 1; return true; }
 *
 *   int value = 10;
 *   mtkx_pipe_stage stages[] = { mtkx_pipe(times_two, NULL),
 *                                mtkx_pipe(plus_one, NULL) };
 *   mtkx_pipe_run(&value, stages, 2);   // value == 21
 * @endcode
 */
#ifndef MEMTKX_DSL_PIPELINE_H
#define MEMTKX_DSL_PIPELINE_H

#include <stddef.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Pipelines                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief A stage: transforms `*value` in place.
 * @return true to continue, false to stop (the value is "none").
 */
typedef bool (*mtkx_pipe_fn)(void *value, void *ctx);

/** A stage function plus its context (C++: `PipeStage<F>`). */
typedef struct mtkx_pipe_stage {
  mtkx_pipe_fn fn;
  void *ctx;
} mtkx_pipe_stage;

/** Builds a stage (C++: `dsl::pipe(f)`). */
MTKX_INLINE mtkx_pipe_stage mtkx_pipe(mtkx_pipe_fn fn, void *ctx) {
  mtkx_pipe_stage stage;
  stage.fn = fn;
  stage.ctx = ctx;
  return stage;
}

/**
 * @brief Runs @p count stages over @p value in order.
 * @return true when every stage returned true; false when one stopped
 *         the pipeline (later stages are not called). Stages with a NULL
 *         function are skipped.
 */
MTKX_API bool mtkx_pipe_run(void *value, const mtkx_pipe_stage *stages, size_t count);

#if !defined(__cplusplus)
/**
 * @brief Convenience: `MTKX_PIPE(&value, mtkx_pipe(f1, c1), mtkx_pipe(f2, c2))`.
 *        (C only: uses a compound literal.)
 */
#define MTKX_PIPE(value, ...)                                                 \
  mtkx_pipe_run((value), (const mtkx_pipe_stage[]){__VA_ARGS__},              \
                sizeof((const mtkx_pipe_stage[]){__VA_ARGS__}) /              \
                    sizeof(mtkx_pipe_stage))
#endif

/* ------------------------------------------------------------------ */
/*  Predicates                                                        */
/* ------------------------------------------------------------------ */

/** A leaf predicate over a value passed by pointer. */
typedef bool (*mtkx_pred_fn)(const void *value, void *ctx);

/** Node kinds of a predicate expression. */
typedef enum mtkx_predicate_kind {
  MTKX_PREDICATE_LEAF = 0, /**< calls fn(value, ctx) */
  MTKX_PREDICATE_AND = 1,  /**< lhs && rhs, short-circuit */
  MTKX_PREDICATE_OR = 2,   /**< lhs || rhs, short-circuit */
  MTKX_PREDICATE_NOT = 3   /**< !lhs */
} mtkx_predicate_kind;

/**
 * @brief A composable predicate (C++: `Predicate<F>`).
 *
 * Composite nodes refer to their operands by pointer; the operands must
 * outlive every use of the composite. Building composites never allocates.
 */
typedef struct mtkx_predicate {
  mtkx_predicate_kind kind;
  mtkx_pred_fn fn;
  void *ctx;
  const struct mtkx_predicate *lhs;
  const struct mtkx_predicate *rhs;
} mtkx_predicate;

/** Wraps a function as a predicate (C++: `dsl::predicate(f)`). */
MTKX_INLINE mtkx_predicate mtkx_predicate_make(mtkx_pred_fn fn, void *ctx) {
  mtkx_predicate p;
  p.kind = MTKX_PREDICATE_LEAF;
  p.fn = fn;
  p.ctx = ctx;
  p.lhs = NULL;
  p.rhs = NULL;
  return p;
}

/** Conjunction (C++: `a & b`). */
MTKX_INLINE mtkx_predicate mtkx_predicate_and(const mtkx_predicate *a,
                                              const mtkx_predicate *b) {
  mtkx_predicate p = mtkx_predicate_make(NULL, NULL);
  p.kind = MTKX_PREDICATE_AND;
  p.lhs = a;
  p.rhs = b;
  return p;
}

/** Disjunction (C++: `a | b`). */
MTKX_INLINE mtkx_predicate mtkx_predicate_or(const mtkx_predicate *a,
                                             const mtkx_predicate *b) {
  mtkx_predicate p = mtkx_predicate_and(a, b);
  p.kind = MTKX_PREDICATE_OR;
  return p;
}

/** Negation (C++: `!a`). */
MTKX_INLINE mtkx_predicate mtkx_predicate_not(const mtkx_predicate *a) {
  mtkx_predicate p = mtkx_predicate_make(NULL, NULL);
  p.kind = MTKX_PREDICATE_NOT;
  p.lhs = a;
  return p;
}

/** Evaluates @p predicate on @p value with short-circuiting. */
MTKX_API bool mtkx_predicate_test(const mtkx_predicate *predicate, const void *value);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_PIPELINE_H */
