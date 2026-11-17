/* Internal C boundary: generated ASDL types never leak into the C++ SDK. */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct qld_expression qld_expression;
typedef enum { QLD_NUMBER, QLD_SYMBOL, QLD_DOT, QLD_UNARY, QLD_BINARY,
               QLD_CONDITIONAL, QLD_CALL } qld_expression_kind;
typedef struct {
  qld_expression_kind kind;
  const char *text;
  size_t length;
  const void *first, *second, *third;
  size_t count;
} qld_expression_view;
/* Serialized by Expression.cpp: Aurocks' error/depth state is not reentrant.
   Every successful or speculative node belongs to this invocation's arena. */
qld_expression *qld_expression_open(const char *input, size_t *consumed);
const void *qld_expression_root(const qld_expression *);
void qld_expression_close(qld_expression *);
qld_expression_view qld_expression_inspect(const void *);
const void *qld_expression_argument(const void *, size_t);
#ifdef __cplusplus
}
#endif
