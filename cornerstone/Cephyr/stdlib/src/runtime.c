/*
 * Minimal process environment, signals, and termination support.
 * The runtime is single-threaded; exit-handler lists are not synchronized.
 */

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * stdio may provide this internal hook.  Keeping it weak lets the low-level
 * runtime link without pulling in or depending on a stdio implementation.
 */
extern void __cephyr_stdio_flush_all(void) __attribute__((weak));
/* A program supplying its own startup may omit the environment vector. */
extern char **environ __attribute__((weak));

/* Linux rt_sigaction takes the kernel layout, not the libc/POSIX layout. */
struct kernel_sigaction {
    __cephyr_signal_handler handler;
    unsigned long flags;
#if defined(__x86_64__) || defined(__aarch64__)
    void (*restorer)(void);
#endif
    unsigned long mask;
};

#define KERNEL_SA_RESTART 0x10000000UL
#define KERNEL_SIG_BLOCK 0
#define KERNEL_SIG_UNBLOCK 1

int __cephyr_rt_sigaction(int sig, const struct kernel_sigaction *action,
                          struct kernel_sigaction *old, size_t mask_size);
int __cephyr_rt_sigprocmask(int how, const unsigned long *mask,
                            unsigned long *old, size_t mask_size);
int __cephyr_getpid(void);
int __cephyr_gettid(void);
int __cephyr_tgkill(int pid, int tid, int sig);
#ifdef __x86_64__
#define KERNEL_SA_RESTORER 0x04000000UL
void __cephyr_rt_sigreturn(void);
#endif

static int supported_signal(int sig)
{
    switch (sig) {
    case SIGINT:
    case SIGILL:
    case SIGABRT:
    case SIGFPE:
    case SIGSEGV:
    case SIGTERM:
        return 1;
    default:
        errno = EINVAL;
        return 0;
    }
}

__cephyr_signal_handler signal(int sig, __cephyr_signal_handler handler)
{
    struct kernel_sigaction action = {0}, old;
    if (!supported_signal(sig))
        return SIG_ERR;
    if (handler == SIG_ERR) {
        errno = EINVAL;
        return SIG_ERR;
    }
    action.handler = handler;
    /* Persistent disposition; Linux blocks the delivered signal in its handler. */
    action.flags = KERNEL_SA_RESTART;
#ifdef __x86_64__
    action.flags |= KERNEL_SA_RESTORER;
    action.restorer = __cephyr_rt_sigreturn;
#endif
    if (__cephyr_rt_sigaction(sig, &action, &old, sizeof action.mask) < 0)
        return SIG_ERR;
    return old.handler;
}

int raise(int sig)
{
    unsigned long bit, old;
    int pid, tid, result;
    if (!supported_signal(sig))
        return -1;
    pid = __cephyr_getpid();
    if (pid < 0)
        return -1;
    tid = __cephyr_gettid();
    if (tid < 0)
        return -1;

    /* An exec'd process can inherit a blocked signal. Send it unblocked so
       any installed handler finishes before raise returns. */
    bit = 1UL << (sig - 1);
    if (__cephyr_rt_sigprocmask(KERNEL_SIG_UNBLOCK, &bit, &old, sizeof bit) < 0)
        return -1;
    result = __cephyr_tgkill(pid, tid, sig);
    if ((old & bit) != 0 &&
        __cephyr_rt_sigprocmask(KERNEL_SIG_BLOCK, &bit, NULL, sizeof bit) < 0)
        return -1;
    return result;
}

#define CEPHYR_ATEXIT_CAPACITY 32

static __cephyr_atexit_function exit_functions[CEPHYR_ATEXIT_CAPACITY];
static __cephyr_atexit_function quick_exit_functions[CEPHYR_ATEXIT_CAPACITY];
static unsigned exit_count;
static unsigned quick_exit_count;

static int register_handler(__cephyr_atexit_function *handlers, unsigned *count,
                            __cephyr_atexit_function function)
{
    if (function == NULL || *count == CEPHYR_ATEXIT_CAPACITY)
        return -1;
    handlers[(*count)++] = function;
    return 0;
}

int atexit(__cephyr_atexit_function function)
{
    return register_handler(exit_functions, &exit_count, function);
}

int at_quick_exit(__cephyr_atexit_function function)
{
    return register_handler(quick_exit_functions, &quick_exit_count, function);
}

void exit(int status)
{
    while (exit_count != 0)
        exit_functions[--exit_count]();
    if (__cephyr_stdio_flush_all != NULL)
        __cephyr_stdio_flush_all();
    _Exit(status);
}

void quick_exit(int status)
{
    while (quick_exit_count != 0)
        quick_exit_functions[--quick_exit_count]();
    _Exit(status);
}

char *getenv(const char *name)
{
    char **entry;
    size_t length;
    if (*name == '\0' || strchr(name, '=') != NULL ||
        &environ == NULL || environ == NULL)
        return NULL;
    length = strlen(name);
    for (entry = environ; *entry != NULL; ++entry) {
        if (strncmp(*entry, name, length) == 0 && (*entry)[length] == '=')
            return *entry + length + 1;
    }
    return NULL;
}
