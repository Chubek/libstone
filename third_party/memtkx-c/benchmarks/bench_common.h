/* Shared timing helper for the benchmarks. */
#ifndef MEMTKX_BENCH_COMMON_H
#define MEMTKX_BENCH_COMMON_H

#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
static uint64_t bench_now_ns(void) {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
#include <time.h>
static uint64_t bench_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

#endif
