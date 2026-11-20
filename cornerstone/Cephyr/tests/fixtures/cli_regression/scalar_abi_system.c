#include <stdarg.h>

typedef long (*callback_t)(signed char, unsigned short, int, unsigned, long, long, long, long, long, long);
extern callback_t exported_callback;
extern long cephyr_abi(callback_t callback);
long external_global = 1234;

long system_variadic(int count, ...)
{
    long total = 0;
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; ++i)
        total += va_arg(ap, long);
    va_end(ap);
    return total;
}

static long reference(signed char a, unsigned short b, int c, unsigned d, long e, long f, long g, long h,
                      long i, long j)
{
    return a + (long)b + c + (long)d + e + f + g + h + i + j;
}

int main(void)
{
    long expected = reference(-7, 65535, -11, 0xffffffffU, 5, 6, 7, 8, 9, 10);
    return exported_callback(-7, 65535, -11, 0xffffffffU, 5, 6, 7, 8, 9, 10) != expected ||
           cephyr_abi(reference) != 2 * expected + 43 + external_global;
}
