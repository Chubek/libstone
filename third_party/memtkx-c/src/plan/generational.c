#include "memtkx/plan/generational.h"

#include <string.h>

#include "memtkx/barrier/card_table.h"
#include "memtkx/space/bump_pointer.h"
#include "memtkx/space/free_list.h"
#include "plan/plan_internal.h"

struct mtkx_generational_plan {
  mtkx_plan base;
  void *nursery_storage;
  size_t nursery_bytes;
  void *mature_storage;
  size_t mature_bytes;
  mtkx_bump_allocator nursery;
  mtkx_free_list_allocator mature;
  mtkx_card_table card_table;
  mtkx__addr_map object_sizes; /* address -> requested size */
  size_t collection_count;
};

static mtkx_plan_kind mtkx__gen_kind(const mtkx_plan *p) {
  (void)p;
  return MTKX_PLAN_GENERATIONAL;
}
static const char *mtkx__gen_name(const mtkx_plan *p) {
  (void)p;
  return "Generational";
}
static mtkx_alloc_result mtkx__gen_allocate(mtkx_plan *p, size_t bytes, size_t alignment) {
  return mtkx_generational_plan_allocate((mtkx_generational_plan *)p, bytes, alignment);
}
static mtkx_plan_stats_result mtkx__gen_collect(mtkx_plan *p, const mtkx_address *roots,
                                                size_t n, mtkx_trace trace) {
  return mtkx_generational_plan_collect((mtkx_generational_plan *)p, roots, n, trace);
}
static void mtkx__gen_destroy(mtkx_plan *p) {
  mtkx_generational_plan_destroy((mtkx_generational_plan *)p);
}

static const mtkx_plan_ops mtkx__gen_ops = {mtkx__gen_kind, mtkx__gen_name,
                                            mtkx__gen_allocate, mtkx__gen_collect,
                                            mtkx__gen_destroy};

mtkx_generational_plan *mtkx_generational_plan_create(size_t nursery_bytes,
                                                      size_t mature_bytes) {
  mtkx_generational_plan *plan =
      (mtkx_generational_plan *)mtkx_calloc(1, sizeof *plan);
  mtkx_address nursery;
  mtkx_address mature;
  if (plan == NULL) {
    return NULL;
  }
  plan->base.ops = &mtkx__gen_ops;
  mtkx__addr_map_init(&plan->object_sizes);
  if (!mtkx__alloc_storage(nursery_bytes, &plan->nursery_storage) ||
      !mtkx__alloc_storage(mature_bytes, &plan->mature_storage)) {
    mtkx_generational_plan_destroy(plan);
    return NULL;
  }
  plan->nursery_bytes = nursery_bytes;
  plan->mature_bytes = mature_bytes;
  nursery = mtkx_as_address(plan->nursery_storage);
  mature = mtkx_as_address(plan->mature_storage);
  mtkx_bump_allocator_init(&plan->nursery, nursery, nursery + nursery_bytes);
  if (mtkx_free_list_allocator_init(&plan->mature, mature, mature + mature_bytes) != MTKX_OK ||
      mtkx_card_table_init(&plan->card_table, mature, mature_bytes,
                           MTKX_DEFAULT_CARD_BYTES) != MTKX_OK) {
    mtkx_generational_plan_destroy(plan);
    return NULL;
  }
  return plan;
}

void mtkx_generational_plan_destroy(mtkx_generational_plan *plan) {
  if (plan == NULL) {
    return;
  }
  mtkx_free_list_allocator_destroy(&plan->mature);
  mtkx_card_table_destroy(&plan->card_table);
  mtkx__addr_map_destroy(&plan->object_sizes);
  mtkx_free(plan->nursery_storage);
  mtkx_free(plan->mature_storage);
  mtkx_free(plan);
}

mtkx_plan *mtkx_generational_plan_as_plan(mtkx_generational_plan *plan) {
  return &plan->base;
}

mtkx_alloc_result mtkx_generational_plan_allocate(mtkx_generational_plan *plan, size_t bytes,
                                                  size_t alignment) {
  mtkx_alloc_result result = mtkx_bump_allocator_allocate(&plan->nursery, bytes, alignment);
  if (result.ok &&
      mtkx__addr_map_emplace(&plan->object_sizes, result.value, bytes, NULL) != MTKX_OK) {
    return mtkx_alloc_result_from_err(MTKX_ALLOC_ALLOCATION_FAILED);
  }
  return result;
}

void mtkx_generational_plan_remember(mtkx_generational_plan *plan,
                                     mtkx_address slot_address) {
  mtkx_card_table_mark(&plan->card_table, slot_address);
}

size_t mtkx_generational_plan_nursery_live_bytes(const mtkx_generational_plan *plan) {
  size_t total = 0;
  size_t i;
  const mtkx_address start = mtkx_bump_allocator_start(&plan->nursery);
  const mtkx_address limit = mtkx_bump_allocator_limit(&plan->nursery);
  for (i = 0; i < plan->object_sizes.capacity; ++i) {
    mtkx_address address;
    if (!plan->object_sizes.used[i]) {
      continue;
    }
    address = plan->object_sizes.keys[i];
    if (limit != 0 && address >= start && address < limit) {
      total += (size_t)plan->object_sizes.values[i];
    }
  }
  return total;
}

size_t mtkx_generational_plan_dirty_cards(const mtkx_generational_plan *plan) {
  return mtkx_card_table_marked_cards(&plan->card_table);
}

mtkx_plan_stats_result mtkx_generational_plan_minor_collect(mtkx_generational_plan *plan,
                                                            const mtkx_address *roots,
                                                            size_t root_count,
                                                            mtkx_trace trace) {
  mtkx_address_vec work = MTKX_ADDRESS_VEC_INIT;
  mtkx__addr_map seen = MTKX__ADDR_MAP_INIT;
  mtkx__addr_map survivors = MTKX__ADDR_MAP_INIT;
  mtkx_plan_stats stats = mtkx__stats_zero();
  mtkx_gc_error error = MTKX_GC_PLAN_UNAVAILABLE;
  size_t i;

  if (!mtkx__seed_roots(&work, roots, root_count)) {
    goto fail;
  }
  while (!mtkx_address_vec_empty(&work)) {
    const mtkx_address object = mtkx_address_vec_pop(&work);
    bool inserted = false;
    if (!mtkx__addr_map_contains(&plan->object_sizes, object)) {
      continue;
    }
    if (mtkx__addr_map_emplace(&seen, object, 1, &inserted) != MTKX_OK) {
      goto fail;
    }
    if (!inserted) {
      continue;
    }
    if (!mtkx__trace_into(trace, object, &work)) {
      error = MTKX_GC_INVALID_TRACE;
      goto fail;
    }
  }

  /* promote every reachable tracked object into mature storage */
  for (i = 0; i < seen.capacity; ++i) {
    mtkx_address object;
    uintptr_t bytes = 0;
    mtkx_alloc_result promoted;
    if (!seen.used[i]) {
      continue;
    }
    object = seen.keys[i];
    (void)mtkx__addr_map_get(&plan->object_sizes, object, &bytes);
    promoted = mtkx_free_list_allocator_allocate(&plan->mature, (size_t)bytes, MTKX_MAX_ALIGN);
    if (!promoted.ok) {
      goto fail; /* earlier copies are not rolled back, as in C++ */
    }
    memcpy(mtkx_from_address(void, promoted.value), mtkx_from_address(void, object),
           (size_t)bytes);
    if (mtkx__addr_map_emplace(&survivors, promoted.value, bytes, NULL) != MTKX_OK) {
      goto fail;
    }
    stats.live_objects += 1;
    stats.live_bytes += (size_t)bytes;
  }

  mtkx__addr_map_swap(&plan->object_sizes, &survivors);
  mtkx_bump_allocator_reset(&plan->nursery);
  mtkx_card_table_clear_all(&plan->card_table);
  ++plan->collection_count;
  stats.collection_count = plan->collection_count;
  mtkx_address_vec_destroy(&work);
  mtkx__addr_map_destroy(&seen);
  mtkx__addr_map_destroy(&survivors);
  return mtkx_plan_stats_result_from_ok(stats);

fail:
  mtkx_address_vec_destroy(&work);
  mtkx__addr_map_destroy(&seen);
  mtkx__addr_map_destroy(&survivors);
  return mtkx_plan_stats_result_from_err(error);
}

mtkx_plan_stats_result mtkx_generational_plan_collect(mtkx_generational_plan *plan,
                                                      const mtkx_address *roots,
                                                      size_t root_count, mtkx_trace trace) {
  return mtkx_generational_plan_minor_collect(plan, roots, root_count, trace);
}
