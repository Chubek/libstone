#ifndef _CEPHYR_SIGNAL_H
#define _CEPHYR_SIGNAL_H

typedef int sig_atomic_t;
typedef void (*__cephyr_signal_handler)(int);

#define SIG_DFL ((__cephyr_signal_handler)0)
#define SIG_IGN ((__cephyr_signal_handler)1)
#define SIG_ERR ((__cephyr_signal_handler)(-1))

/* Signal numbers of the supported Linux LP64 targets. */
#define SIGINT 2
#define SIGILL 4
#define SIGABRT 6
#define SIGFPE 8
#define SIGSEGV 11
#define SIGTERM 15

__cephyr_signal_handler signal(int, __cephyr_signal_handler);
int raise(int);

#endif
