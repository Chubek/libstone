#include "memtkx/dsl/pipeline.h"

bool mtkx_pipe_run(void *value, const mtkx_pipe_stage *stages, size_t count) {
  size_t i;
  for (i = 0; i < count; ++i) {
    if (stages[i].fn != NULL && !stages[i].fn(value, stages[i].ctx)) {
      return false;
    }
  }
  return true;
}

bool mtkx_predicate_test(const mtkx_predicate *predicate, const void *value) {
  if (predicate == NULL) {
    return false;
  }
  switch (predicate->kind) {
    case MTKX_PREDICATE_LEAF:
      return predicate->fn != NULL && predicate->fn(value, predicate->ctx);
    case MTKX_PREDICATE_AND:
      return mtkx_predicate_test(predicate->lhs, value) &&
             mtkx_predicate_test(predicate->rhs, value);
    case MTKX_PREDICATE_OR:
      return mtkx_predicate_test(predicate->lhs, value) ||
             mtkx_predicate_test(predicate->rhs, value);
    case MTKX_PREDICATE_NOT:
      return !mtkx_predicate_test(predicate->lhs, value);
  }
  return false;
}
