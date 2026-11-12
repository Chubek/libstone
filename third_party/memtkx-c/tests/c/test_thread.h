/* Tiny portable thread shim for tests (POSIX threads or Win32). */
#ifndef MEMTKX_TEST_THREAD_H
#define MEMTKX_TEST_THREAD_H

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE test_thread;
typedef struct test_thread_start_ {
  void (*fn)(void *);
  void *arg;
} test_thread_start_;
static DWORD WINAPI test_thread_trampoline_(LPVOID p) {
  test_thread_start_ s = *(test_thread_start_ *)p;
  HeapFree(GetProcessHeap(), 0, p);
  s.fn(s.arg);
  return 0;
}
static int test_thread_create(test_thread *t, void (*fn)(void *), void *arg) {
  test_thread_start_ *s =
      (test_thread_start_ *)HeapAlloc(GetProcessHeap(), 0, sizeof *s);
  if (s == NULL) return -1;
  s->fn = fn;
  s->arg = arg;
  *t = CreateThread(NULL, 0, test_thread_trampoline_, s, 0, NULL);
  return *t != NULL ? 0 : -1;
}
static void test_thread_join(test_thread t) {
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}
static void test_sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
typedef pthread_t test_thread;
typedef struct test_thread_start_ {
  void (*fn)(void *);
  void *arg;
} test_thread_start_;
static void *test_thread_trampoline_(void *p) {
  test_thread_start_ s = *(test_thread_start_ *)p;
  free(p);
  s.fn(s.arg);
  return NULL;
}
static int test_thread_create(test_thread *t, void (*fn)(void *), void *arg) {
  test_thread_start_ *s = (test_thread_start_ *)malloc(sizeof *s);
  if (s == NULL) return -1;
  s->fn = fn;
  s->arg = arg;
  return pthread_create(t, NULL, test_thread_trampoline_, s);
}
static void test_thread_join(test_thread t) { pthread_join(t, NULL); }
static void test_sleep_ms(unsigned ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
  nanosleep(&ts, NULL);
}
#endif

#endif
