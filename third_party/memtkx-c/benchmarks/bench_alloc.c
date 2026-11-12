/* Port of benchmarks/bench_alloc.cpp: CSV of ns per allocation-loop iteration. */
#include <stdio.h>
#include <stdlib.h>

#include <memtkx/memtkx.h>

#include "bench_common.h"

#define STORAGE_BYTES (16u * 1024u * 1024u)

static double run_bump(size_t iterations) {
  void *storage = malloc(STORAGE_BYTES);
  mtkx_bump_allocator allocator;
  uint64_t start, stop;
  size_t i;
  if (storage == NULL) {
    return -1.0;
  }
  mtkx_bump_allocator_init(&allocator, mtkx_as_address(storage),
                           mtkx_as_address(storage) + STORAGE_BYTES);
  start = bench_now_ns();
  for (i = 0; i < iterations; ++i) {
    mtkx_bump_allocator_reset(&allocator);
    if (!mtkx_bump_allocator_allocate(&allocator, 32, 8).ok) {
      free(storage);
      return -1.0;
    }
  }
  stop = bench_now_ns();
  free(storage);
  return (double)(stop - start) / (double)iterations;
}

/* Same loop through the header-inline fast path. */
static double run_bump_inline(size_t iterations) {
  void *storage = malloc(STORAGE_BYTES);
  mtkx_bump_allocator allocator;
  volatile mtkx_address sink = 0; /* keep the allocation observable */
  uint64_t start, stop;
  size_t i;
  if (storage == NULL) {
    return -1.0;
  }
  mtkx_bump_allocator_init(&allocator, mtkx_as_address(storage),
                           mtkx_as_address(storage) + STORAGE_BYTES);
  start = bench_now_ns();
  for (i = 0; i < iterations; ++i) {
    mtkx_bump_allocator_reset(&allocator);
    sink = mtkx_bump_allocator_try_allocate(&allocator, 32, 8);
    if (sink == 0) {
      free(storage);
      return -1.0;
    }
  }
  stop = bench_now_ns();
  free(storage);
  return (double)(stop - start) / (double)iterations;
}

static double run_free_list(size_t iterations) {
  void *storage = malloc(STORAGE_BYTES);
  mtkx_free_list_allocator allocator;
  uint64_t start, stop;
  size_t i;
  if (storage == NULL ||
      mtkx_free_list_allocator_init(&allocator, mtkx_as_address(storage),
                                    mtkx_as_address(storage) + STORAGE_BYTES) != MTKX_OK) {
    free(storage);
    return -1.0;
  }
  start = bench_now_ns();
  for (i = 0; i < iterations; ++i) {
    mtkx_alloc_result result = mtkx_free_list_allocator_allocate(&allocator, 32, 8);
    if (!result.ok) {
      mtkx_free_list_allocator_destroy(&allocator);
      free(storage);
      return -1.0;
    }
    (void)mtkx_free_list_allocator_free(&allocator, result.value, 32);
  }
  stop = bench_now_ns();
  mtkx_free_list_allocator_destroy(&allocator);
  free(storage);
  return (double)(stop - start) / (double)iterations;
}

static double run_immix(size_t iterations) {
  mtkx_immix_space space;
  uint64_t start, stop;
  size_t i;
  if (mtkx_immix_space_init(&space, MTKX_IMMIX_BLOCK_BYTES * 4) != MTKX_OK) {
    return -1.0;
  }
  start = bench_now_ns();
  for (i = 0; i < iterations; ++i) {
    if (i % 256 == 0) {
      mtkx_immix_space_reset_blocks(&space);
    }
    if (!mtkx_immix_space_allocate(&space, 32, 8).ok) {
      mtkx_immix_space_destroy(&space);
      return -1.0;
    }
  }
  stop = bench_now_ns();
  mtkx_immix_space_destroy(&space);
  return (double)(stop - start) / (double)iterations;
}

int main(void) {
  const size_t iterations = 200000;
  const double bump_ns = run_bump(iterations);
  const double bump_inline_ns = run_bump_inline(iterations);
  const double free_list_ns = run_free_list(iterations);
  const double immix_ns = run_immix(iterations);

  printf("allocator,ns_per_operation\n");
  printf("bump-pointer,%g\n", bump_ns);
  printf("bump-pointer-inline,%g\n", bump_inline_ns);
  printf("free-list,%g\n", free_list_ns);
  printf("immix,%g\n", immix_ns);
  return bump_ns < 0.0 || bump_inline_ns < 0.0 || free_list_ns < 0.0 || immix_ns < 0.0;
}
