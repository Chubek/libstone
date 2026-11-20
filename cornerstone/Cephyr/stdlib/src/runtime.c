/*
 * Minimal process termination support.  This runtime is intentionally
 * single-threaded: atexit registration and execution are not synchronized.
 */

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
