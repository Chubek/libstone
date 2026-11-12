/**
 * @file memtkx/dsl/expr.h
 * @brief Lazy arithmetic expression trees (C++ `ExprTemplates`).
 *
 * Building an expression performs no arithmetic and allocates nothing:
 * nodes are small values that point at their operands. Evaluation walks
 * the tree once. Terminals are constants, callbacks (C++ `expr_value()`)
 * or arrays; arrays enable fused element-wise evaluation.
 *
 * @code
 *   mtkx_expr a = mtkx_expr_const(3.0), b = mtkx_expr_const(4.0);
 *   mtkx_expr bxa = mtkx_expr_mul(&b, &a);
 *   mtkx_expr sum = mtkx_expr_add(&a, &bxa);       // lazy: a + b * a
 *   double r = mtkx_expr_eval(&sum);               // 15.0
 *
 *   double x[] = {1, 2, 3}, y[] = {4, 5, 6}, z[] = {7, 8, 9}, out[3];
 *   mtkx_expr ex = mtkx_expr_array(x, 3), ey = mtkx_expr_array(y, 3),
 *             ez = mtkx_expr_array(z, 3);
 *   mtkx_expr yz = mtkx_expr_mul(&ey, &ez);
 *   mtkx_expr v = mtkx_expr_add(&ex, &yz);
 *   mtkx_expr_eval_into(&v, out, 3);               // {29, 42, 57}
 * @endcode
 *
 * Operands must outlive the expressions that refer to them.
 */
#ifndef MEMTKX_DSL_EXPR_H
#define MEMTKX_DSL_EXPR_H

#include <stddef.h>

#include "memtkx/core/base.h"

MTKX_BEGIN_DECLS

/** Expression node kinds. */
typedef enum mtkx_expr_kind {
  MTKX_EXPR_CONST = 0,   /**< a constant double */
  MTKX_EXPR_VALUE = 1,   /**< value_fn(self) */
  MTKX_EXPR_ARRAY = 2,   /**< data[i] */
  MTKX_EXPR_ADD = 3,
  MTKX_EXPR_SUB = 4,
  MTKX_EXPR_MUL = 5,
  MTKX_EXPR_DIV = 6,
  MTKX_EXPR_NEG = 7      /**< unary minus of lhs */
} mtkx_expr_kind;

/** Callback terminal (C++ `Derived::expr_value()`). */
typedef double (*mtkx_expr_value_fn)(const void *self);

/** An expression node. */
typedef struct mtkx_expr {
  mtkx_expr_kind kind;
  double constant;
  mtkx_expr_value_fn value_fn;
  const void *self;
  const double *data;
  size_t length;
  const struct mtkx_expr *lhs;
  const struct mtkx_expr *rhs;
} mtkx_expr;

/** A constant terminal. */
MTKX_INLINE mtkx_expr mtkx_expr_const(double value) {
  mtkx_expr e;
  e.kind = MTKX_EXPR_CONST;
  e.constant = value;
  e.value_fn = NULL;
  e.self = NULL;
  e.data = NULL;
  e.length = 0;
  e.lhs = NULL;
  e.rhs = NULL;
  return e;
}

/** A terminal whose value is `value_fn(self)`, read at evaluation time. */
MTKX_INLINE mtkx_expr mtkx_expr_terminal(mtkx_expr_value_fn value_fn, const void *self) {
  mtkx_expr e = mtkx_expr_const(0.0);
  e.kind = MTKX_EXPR_VALUE;
  e.value_fn = value_fn;
  e.self = self;
  return e;
}

/** An array terminal of @p length elements (borrowed). */
MTKX_INLINE mtkx_expr mtkx_expr_array(const double *data, size_t length) {
  mtkx_expr e = mtkx_expr_const(0.0);
  e.kind = MTKX_EXPR_ARRAY;
  e.data = data;
  e.length = length;
  return e;
}

/** @internal Builds a binary node. */
MTKX_INLINE mtkx_expr mtkx_expr_binary_(mtkx_expr_kind kind, const mtkx_expr *lhs,
                                        const mtkx_expr *rhs) {
  mtkx_expr e = mtkx_expr_const(0.0);
  e.kind = kind;
  e.lhs = lhs;
  e.rhs = rhs;
  return e;
}

/** lhs + rhs (lazy). */
MTKX_INLINE mtkx_expr mtkx_expr_add(const mtkx_expr *lhs, const mtkx_expr *rhs) {
  return mtkx_expr_binary_(MTKX_EXPR_ADD, lhs, rhs);
}
/** lhs - rhs (lazy). */
MTKX_INLINE mtkx_expr mtkx_expr_sub(const mtkx_expr *lhs, const mtkx_expr *rhs) {
  return mtkx_expr_binary_(MTKX_EXPR_SUB, lhs, rhs);
}
/** lhs * rhs (lazy). */
MTKX_INLINE mtkx_expr mtkx_expr_mul(const mtkx_expr *lhs, const mtkx_expr *rhs) {
  return mtkx_expr_binary_(MTKX_EXPR_MUL, lhs, rhs);
}
/** lhs / rhs (lazy). */
MTKX_INLINE mtkx_expr mtkx_expr_div(const mtkx_expr *lhs, const mtkx_expr *rhs) {
  return mtkx_expr_binary_(MTKX_EXPR_DIV, lhs, rhs);
}
/** -operand (lazy). */
MTKX_INLINE mtkx_expr mtkx_expr_neg(const mtkx_expr *operand) {
  return mtkx_expr_binary_(MTKX_EXPR_NEG, operand, NULL);
}

/** Evaluates a scalar expression (array terminals contribute element 0). */
MTKX_API double mtkx_expr_eval(const mtkx_expr *expr);

/**
 * @brief Evaluates element @p index: arrays contribute `data[index]`;
 *        constants and callbacks are broadcast.
 */
MTKX_API double mtkx_expr_eval_at(const mtkx_expr *expr, size_t index);

/**
 * @return the shortest array length in the tree, or 1 when the tree has
 *         no array terminals (a scalar).
 */
MTKX_API size_t mtkx_expr_length(const mtkx_expr *expr);

/**
 * @brief Fused element-wise evaluation into @p out[0..count).
 * @return MTKX_ERR_INVALID_ARGUMENT when @p count exceeds an array length.
 */
MTKX_API mtkx_status mtkx_expr_eval_into(const mtkx_expr *expr, double *out, size_t count);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_EXPR_H */
