#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static volatile sig_atomic_t received;

static void remember(int sig)
{
    received = sig;
}

static void remember_external(int sig)
{
    /* C11 permits reinstalling the disposition of this signal in its handler. */
    received = signal(sig, remember_external) == remember_external ? sig : -sig;
}

static void terminate(int sig)
{
    _Exit(40 + sig);
}

static void acknowledge_abort(int sig)
{
    if (sig == SIGABRT)
        _Exit(42);
    _Exit(43);
}

static void mark_abort(int sig)
{
    if (sig == SIGABRT)
        fputc('A', stderr);
}

/* Test-only use of the packaged Linux mask wrapper. */
int __cephyr_rt_sigprocmask(int, const unsigned long *, unsigned long *, size_t);

int main(int argc, char **argv)
{
    if (argc == 2) {
        if (argv[1][0] == 'd') {
            if (signal(SIGTERM, SIG_DFL) == SIG_ERR)
                return 10;
            raise(SIGTERM);
        } else if (argv[1][0] == 'x') {
            if (signal(SIGABRT, acknowledge_abort) == SIG_ERR)
                return 11;
            abort();
        } else if (argv[1][0] == 'r') {
            if (signal(SIGABRT, mark_abort) == SIG_ERR)
                return 12;
            abort();
        } else if (argv[1][0] == 'i') {
            if (signal(SIGABRT, SIG_IGN) == SIG_ERR)
                return 13;
            abort();
        } else if (argv[1][0] == 'e') {
            if (signal(SIGTERM, remember_external) == SIG_ERR)
                return 14;
            puts("ready");
            fflush(stdout);
            while (received == 0) {
            }
            return received == SIGTERM ? 0 : 15;
        } else {
            int sig = argv[1][0] == 'f' ? SIGFPE :
                      argv[1][0] == 'l' ? SIGILL : SIGSEGV;
            if (signal(sig, terminate) == SIG_ERR)
                return 16;
            raise(sig);
        }
        return 17;
    }

    _Static_assert(SIG_ATOMIC_MIN == INT_MIN && SIG_ATOMIC_MAX == INT_MAX,
                   "Linux signal word has the range of int");
    _Static_assert(sizeof(sig_atomic_t) == sizeof(int), "Linux signal word size");

    if (signal(SIGTERM, SIG_DFL) == SIG_ERR || signal(SIGTERM, remember) != SIG_DFL)
        return 1;
    if (raise(SIGTERM) || received != SIGTERM)
        return 2;
    received = 0;
    if (raise(SIGTERM) || received != SIGTERM)
        return 3; /* Not reset after the first invocation. */
    if (signal(SIGTERM, SIG_IGN) != remember)
        return 4;
    received = 0;
    if (raise(SIGTERM) || received != 0 || signal(SIGTERM, SIG_DFL) != SIG_IGN)
        return 5;

    if (signal(SIGINT, remember) == SIG_ERR)
        return 6;
    unsigned long bit = 1UL << (SIGINT - 1);
    unsigned long mask = 0;
    if (__cephyr_rt_sigprocmask(0, &bit, NULL, sizeof bit) || raise(SIGINT) ||
        received != SIGINT ||
        __cephyr_rt_sigprocmask(1, &bit, &mask, sizeof bit) || (mask & bit) == 0)
        return 7;

    errno = 0;
    if (signal(SIGINT, SIG_ERR) != SIG_ERR || errno != EINVAL ||
        signal(SIGINT, SIG_DFL) != remember)
        return 8;
    errno = 0;
    if (signal(0, SIG_IGN) != SIG_ERR || errno != EINVAL)
        return 9;
    errno = 0;
    if (raise(65) == 0 || errno != EINVAL)
        return 18;
    return 0;
}
