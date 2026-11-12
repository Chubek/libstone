#include "memtkx/dsl/expr.h"

#include <stdint.h>

double mtkx_expr_eval_at(const mtkx_expr *expr, size_t index) {
  switch (expr->kind) {
    case MTKX_EXPR_CONST:
      return expr->constant;
    case MTKX_EXPR_VALUE:
      return expr->value_fn != NULL ? expr->value_fn(expr->self) : 0.0;
    case MTKX_EXPR_ARRAY:
      return index < expr->length ? expr->data[index] : 0.0;
    case MTKX_EXPR_ADD:
      return mtkx_expr_eval_at(expr->lhs, index) + mtkx_expr_eval_at(expr->rhs, index);
    case MTKX_EXPR_SUB:
      return mtkx_expr_eval_at(expr->lhs, index) - mtkx_expr_eval_at(expr->rhs, index);
    case MTKX_EXPR_MUL:
      return mtkx_expr_eval_at(expr->lhs, index) * mtkx_expr_eval_at(expr->rhs, index);
    case MTKX_EXPR_DIV:
      return mtkx_expr_eval_at(expr->lhs, index) / mtkx_expr_eval_at(expr->rhs, index);
    case MTKX_EXPR_NEG:
      return -mtkx_expr_eval_at(expr->lhs, index);
  }
  return 0.0;
}

double mtkx_expr_eval(const mtkx_expr *expr) { return mtkx_expr_eval_at(expr, 0); }

static size_t mtkx__expr_min_length(const mtkx_expr *expr) {
  size_t l;
  size_t r;
  switch (expr->kind) {
    case MTKX_EXPR_CONST:
    case MTKX_EXPR_VALUE:
      return SIZE_MAX;
    case MTKX_EXPR_ARRAY:
      return expr->length;
    case MTKX_EXPR_NEG:
      return mtkx__expr_min_length(expr->lhs);
    case MTKX_EXPR_ADD:
    case MTKX_EXPR_SUB:
    case MTKX_EXPR_MUL:
    case MTKX_EXPR_DIV:
      l = mtkx__expr_min_length(expr->lhs);
      r = mtkx__expr_min_length(expr->rhs);
      return l < r ? l : r;
  }
  return SIZE_MAX;
}

size_t mtkx_expr_length(const mtkx_expr *expr) {
  size_t n = mtkx__expr_min_length(expr);
  return n == SIZE_MAX ? 1u : n;
}

mtkx_status mtkx_expr_eval_into(const mtkx_expr *expr, double *out, size_t count) {
  size_t i;
  size_t limit = mtkx__expr_min_length(expr);
  if (count > limit) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  for (i = 0; i < count; ++i) {
    out[i] = mtkx_expr_eval_at(expr, i);
  }
  return MTKX_OK;
}
