/* Port of benchmarks/bench_barrier.cpp: recording pipeline vs. raw filter. */
#include <stdio.h>

#include <memtkx/memtkx.h>

#include "bench_common.h"

static double run_pipeline(size_t iterations) {
  mtkx_address slot_storage = 0;
  const mtkx_address_range source =
      mtkx_range(mtkx_as_address(&slot_storage), mtkx_as_address(&slot_storage) + 1);
  const mtkx_address_range target = mtkx_range(0x1000, 0x2000);
  mtkx_slot_buffer buffer;
  uint64_t start, stop;
  size_t i;

  mtkx_slot_buffer_init(&buffer);
  start = bench_now_ns();
  for (i = 0; i < iterations; ++i) {
    (void)mtkx_run_write_barrier(mtkx_slot_make(&slot_storage, 0x1800), source, target,
                                 &buffer);
  }
  stop = bench_now_ns();
  mtkx_slot_buffer_destroy(&buffer);
  return (double)(stop - start) / (double)iterations;
}

static double run_raw_filter(size_t iterations) {
  mtkx_address slot_storage = 0;
  const mtkx_address_range source =
      mtkx_range(mtkx_as_address(&slot_storage), mtkx_as_address(&slot_storage) + 1);
  const mtkx_address_range target = mtkx_range(0x1000, 0x2000);
  volatile size_t hits = 0; /* observable, unlike the C++ original */
  uint64_t start, stop;
  size_t i;

  start = bench_now_ns();
  for (i = 0; i < iterations; ++i) {
    const mtkx_address slot = mtkx_as_address(&slot_storage);
    const mtkx_address object = 0x1800;
    if (mtkx_range_contains(source, slot) && mtkx_range_contains(target, object)) {
      hits = hits + 1;
    }
  }
  stop = bench_now_ns();
  return (double)(stop - start) / (double)iterations;
}

int main(void) {
  const size_t iterations = 1000000;
  const double pipeline_ns = run_pipeline(iterations);
  const double raw_ns = run_raw_filter(iterations);

  printf("barrier,ns_per_operation\n");
  printf("pipeline,%g\n", pipeline_ns);
  printf("raw-filter,%g\n", raw_ns);
  return 0;
}
