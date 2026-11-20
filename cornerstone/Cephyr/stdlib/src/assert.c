#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void write_decimal(unsigned value)
{
    char reversed[16];
    unsigned count = 0;

    do {
        reversed[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
        fputc(reversed[--count], stderr);
}

_Noreturn void __cephyr_assert_fail(const char *expression, const char *file,
                                    int line, const char *function)
{
    fputs(file, stderr);
    fputc(':', stderr);
    write_decimal(line < 0 ? 0U : (unsigned)line);
    if (function != NULL) {
        fputs(": ", stderr);
        fputs(function, stderr);
    }
    fputs(": assertion failed: ", stderr);
    fputs(expression, stderr);
    fputc('\n', stderr);
    abort();
}

_Noreturn void __cephyr_assert_fail_at(const char *expression, const char *file,
                                      int line)
{
    __cephyr_assert_fail(expression, file, line, NULL);
}
