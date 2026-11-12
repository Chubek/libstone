#include "memtkx/space/space.h"

const char *mtkx_space_kind_string(mtkx_space_kind kind) {
  switch (kind) {
    case MTKX_SPACE_BUMP_POINTER:
      return "bump-pointer";
    case MTKX_SPACE_FREE_LIST:
      return "free-list";
    case MTKX_SPACE_IMMIX:
      return "immix";
    case MTKX_SPACE_LARGE_OBJECT:
      return "large-object";
    case MTKX_SPACE_NURSERY:
      return "nursery";
    case MTKX_SPACE_SEMI_SPACE:
      return "semi-space";
  }
  return "unknown";
}

mtkx_status mtkx_space_init(mtkx_space *space, const mtkx_space_ops *ops, const char *name,
                            mtkx_space_kind kind, mtkx_address_range region,
                            size_t default_alignment) {
  space->ops = ops;
  space->descriptor.name = mtkx_strdup(name != NULL ? name : "");
  space->descriptor.kind = kind;
  space->descriptor.region = region;
  space->descriptor.default_alignment = default_alignment;
  mtkx_space_metrics_reset(&space->metrics);
  return space->descriptor.name != NULL ? MTKX_OK : MTKX_ERR_NOMEM;
}

void mtkx_space_destroy(mtkx_space *space) {
  if (space == NULL) {
    return;
  }
  if (space->ops != NULL && space->ops->destroy != NULL) {
    space->ops->destroy(space);
  }
  mtkx_free(space->descriptor.name);
  space->descriptor.name = NULL;
}

mtkx_alloc_result mtkx_space_allocate(mtkx_space *space, size_t bytes, size_t alignment) {
  if (!mtkx_is_power_of_two(alignment)) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_MISALIGNED);
  }
  if (space->ops == NULL || space->ops->allocate_impl == NULL) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  return space->ops->allocate_impl(space, bytes, alignment);
}
