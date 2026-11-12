#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "internal/platform.h"

#include <stdint.h>

#if !defined(_WIN32)
#include <errno.h>
#include <time.h>
#endif

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

/* ------------------------------------------------------------------ */
/*  Clock                                                             */
/* ------------------------------------------------------------------ */

uint64_t mtkx__monotonic_ns(void) {
#if defined(_WIN32)
  static LARGE_INTEGER frequency; /* benign race: always the same value */
  LARGE_INTEGER counter;
  if (frequency.QuadPart == 0) {
    QueryPerformanceFrequency(&frequency);
  }
  QueryPerformanceCounter(&counter);
  return (uint64_t)((counter.QuadPart / frequency.QuadPart) * 1000000000ULL +
                    ((counter.QuadPart % frequency.QuadPart) * 1000000000ULL) /
                        frequency.QuadPart);
#elif defined(__APPLE__)
  static mach_timebase_info_data_t info;
  if (info.denom == 0) {
    mach_timebase_info(&info);
  }
  return (uint64_t)mach_absolute_time() * info.numer / info.denom;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

uint64_t mtkx__deadline_after_ms(uint64_t ms) {
  uint64_t now = mtkx__monotonic_ns();
  uint64_t delta;
  if (ms > UINT64_MAX / 1000000ULL) {
    return UINT64_MAX;
  }
  delta = ms * 1000000ULL;
  return now > UINT64_MAX - delta ? UINT64_MAX : now + delta;
}

/* ------------------------------------------------------------------ */
/*  Windows                                                           */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)

mtkx_status mtkx__mutex_init(mtkx__mutex *mutex) {
  InitializeSRWLock(&mutex->lock);
  return MTKX_OK;
}
void mtkx__mutex_destroy(mtkx__mutex *mutex) { (void)mutex; }
void mtkx__mutex_lock(mtkx__mutex *mutex) { AcquireSRWLockExclusive(&mutex->lock); }
void mtkx__mutex_unlock(mtkx__mutex *mutex) { ReleaseSRWLockExclusive(&mutex->lock); }

mtkx_status mtkx__cond_init(mtkx__cond *cond) {
  InitializeConditionVariable(&cond->cv);
  return MTKX_OK;
}
void mtkx__cond_destroy(mtkx__cond *cond) { (void)cond; }
void mtkx__cond_broadcast(mtkx__cond *cond) { WakeAllConditionVariable(&cond->cv); }

bool mtkx__cond_wait_until(mtkx__cond *cond, mtkx__mutex *mutex, uint64_t deadline_ns) {
  uint64_t now = mtkx__monotonic_ns();
  uint64_t remaining_ms;
  if (now >= deadline_ns) {
    return false;
  }
  remaining_ms = (deadline_ns - now + 999999ULL) / 1000000ULL;
  if (remaining_ms >= (uint64_t)INFINITE) {
    remaining_ms = (uint64_t)INFINITE - 1;
  }
  SleepConditionVariableSRW(&cond->cv, &mutex->lock, (DWORD)remaining_ms, 0);
  return mtkx__monotonic_ns() < deadline_ns;
}

/* ------------------------------------------------------------------ */
/*  POSIX                                                             */
/* ------------------------------------------------------------------ */

#else

mtkx_status mtkx__mutex_init(mtkx__mutex *mutex) {
  return pthread_mutex_init(&mutex->mutex, NULL) == 0 ? MTKX_OK : MTKX_ERR_SYSTEM;
}
void mtkx__mutex_destroy(mtkx__mutex *mutex) { pthread_mutex_destroy(&mutex->mutex); }
void mtkx__mutex_lock(mtkx__mutex *mutex) { pthread_mutex_lock(&mutex->mutex); }
void mtkx__mutex_unlock(mtkx__mutex *mutex) { pthread_mutex_unlock(&mutex->mutex); }

mtkx_status mtkx__cond_init(mtkx__cond *cond) {
#if defined(__APPLE__)
  return pthread_cond_init(&cond->cond, NULL) == 0 ? MTKX_OK : MTKX_ERR_SYSTEM;
#else
  pthread_condattr_t attr;
  int rc;
  if (pthread_condattr_init(&attr) != 0) {
    return MTKX_ERR_SYSTEM;
  }
  /* time out against the monotonic clock, immune to wall-clock changes */
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  rc = pthread_cond_init(&cond->cond, &attr);
  pthread_condattr_destroy(&attr);
  return rc == 0 ? MTKX_OK : MTKX_ERR_SYSTEM;
#endif
}
void mtkx__cond_destroy(mtkx__cond *cond) { pthread_cond_destroy(&cond->cond); }
void mtkx__cond_broadcast(mtkx__cond *cond) { pthread_cond_broadcast(&cond->cond); }

bool mtkx__cond_wait_until(mtkx__cond *cond, mtkx__mutex *mutex, uint64_t deadline_ns) {
  struct timespec ts;
  uint64_t now = mtkx__monotonic_ns();
  if (now >= deadline_ns) {
    return false;
  }
#if defined(__APPLE__)
  {
    uint64_t remaining = deadline_ns - now;
    ts.tv_sec = (time_t)(remaining / 1000000000ULL);
    ts.tv_nsec = (long)(remaining % 1000000000ULL);
    pthread_cond_timedwait_relative_np(&cond->cond, &mutex->mutex, &ts);
  }
#else
  {
    uint64_t sec = deadline_ns / 1000000000ULL;
    /* clamp absurd deadlines to something time_t can hold */
    if (sec > (uint64_t)INT32_MAX) {
      sec = (uint64_t)INT32_MAX;
    }
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)(deadline_ns % 1000000000ULL);
    pthread_cond_timedwait(&cond->cond, &mutex->mutex, &ts);
  }
#endif
  return mtkx__monotonic_ns() < deadline_ns;
}

#endif
