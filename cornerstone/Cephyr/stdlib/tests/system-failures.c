/* Linker-wrapped syscalls exercise errors without relying on privileged
   chroots, process limits, or a missing /bin/sh on the test host. */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>

static int deny_clone, interrupt_wait, lose_status;
int __real___cephyr_clone(unsigned long, void *, void *, void *, void *);
int __real___cephyr_wait4(int, int *, int, void *);

int __wrap___cephyr_clone(unsigned long flags, void *sp, void *ptid, void *a, void *b)
{
    if (!deny_clone)
        return __real___cephyr_clone(flags, sp, ptid, a, b);
    errno = EAGAIN;
    return -1;
}

int __wrap___cephyr_execve(const char *path, const char *const *argv, char *const *envp)
{
    (void)path; (void)argv; (void)envp;
    errno = ENOENT;
    return -1;
}

int __wrap___cephyr_wait4(int pid, int *status, int opts, void *usage)
{
    if (interrupt_wait) {
        interrupt_wait = 0;
        errno = EINTR;
        return -1;
    }
    int r = __real___cephyr_wait4(pid, status, opts, usage);
    if (r == pid && lose_status) {
        errno = ECHILD;
        return -1;
    }
    return r;
}

static void child_must_not_call_exit(void) { _Exit(44); }

int main(void)
{
    if (atexit(child_must_not_call_exit) || signal(SIGINT, SIG_DFL) == SIG_ERR)
        _Exit(1);
    interrupt_wait = 1;
    if (system(NULL) || interrupt_wait || system("exit 0") != (127 << 8))
        _Exit(2);
    lose_status = 1;
    errno = 0;
    if (system(":") != -1 || errno != ECHILD)
        _Exit(4);
    deny_clone = 1;
    errno = 0;
    if (system(":") != -1 || errno != EAGAIN || system(NULL) ||
        signal(SIGINT, SIG_DFL) != SIG_DFL)
        _Exit(3);
    _Exit(0);
}
