/*
 * Minimal process termination support.  This runtime is intentionally
 * single-threaded: atexit registration and execution are not synchronized.
 */

typedef void (*cephyr_exit_function)(void);

extern void _Exit(int status) __attribute__((noreturn));

/*
 * stdio may provide this internal hook.  Keeping it weak lets the low-level
 * runtime link without pulling in or depending on a stdio implementation.
 */
extern void __cephyr_stdio_flush_all(void) __attribute__((weak));

#define CEPHYR_ATEXIT_CAPACITY 32

static cephyr_exit_function cephyr_atexit_functions[CEPHYR_ATEXIT_CAPACITY];
static unsigned int cephyr_atexit_count;

int atexit(void (*function)(void)) {
    if (function == (cephyr_exit_function)0 ||
        cephyr_atexit_count == CEPHYR_ATEXIT_CAPACITY) {
        return -1;
    }

    cephyr_atexit_functions[cephyr_atexit_count++] = function;
    return 0;
}

void exit(int status) {
    while (cephyr_atexit_count != 0) {
        cephyr_exit_function function =
            cephyr_atexit_functions[--cephyr_atexit_count];
        function();
    }

    if (__cephyr_stdio_flush_all != (void (*)(void))0) {
        __cephyr_stdio_flush_all();
    }

    _Exit(status);
}
