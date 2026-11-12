/*
 * Internal portability layer: mutex, condition variable and a monotonic
 * clock for POSIX (Linux/BSD/macOS) and Windows.
 */
#ifndef MEMTKX_INTERNAL_PLATFORM_H
#define MEMTKX_INTERNAL_PLATFORM_H

#include <stdint.h>

#include "memtkx/core/base.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct mtkx__mutex {
  SRWLOCK lock;
} mtkx__mutex;
typedef struct mtkx__cond {
  CONDITION_VARIABLE cv;
} mtkx__cond;
#else
#include <pthread.h>
typedef struct mtkx__mutex {
  pthread_mutex_t mutex;
} mtkx__mutex;
typedef struct mtkx__cond {
  pthread_cond_t cond;
} mtkx__cond;
#endif

mtkx_status mtkx__mutex_init(mtkx__mutex *mutex);
void mtkx__mutex_destroy(mtkx__mutex *mutex);
void mtkx__mutex_lock(mtkx__mutex *mutex);
void mtkx__mutex_unlock(mtkx__mutex *mutex);

mtkx_status mtkx__cond_init(mtkx__cond *cond);
void mtkx__cond_destroy(mtkx__cond *cond);
void mtkx__cond_broadcast(mtkx__cond *cond);

/*
 * Waits on cond (mutex held) until signalled, spuriously woken, or the
 * monotonic clock reaches deadline_ns. Returns false once the deadline has
 * passed, true otherwise. Callers re-check their predicate in a loop.
 */
bool mtkx__cond_wait_until(mtkx__cond *cond, mtkx__mutex *mutex, uint64_t deadline_ns);

/* Nanoseconds from an arbitrary fixed point; never goes backwards. */
uint64_t mtkx__monotonic_ns(void);

/* deadline = now + ms, saturating. */
uint64_t mtkx__deadline_after_ms(uint64_t ms);

#endif /* MEMTKX_INTERNAL_PLATFORM_H */
