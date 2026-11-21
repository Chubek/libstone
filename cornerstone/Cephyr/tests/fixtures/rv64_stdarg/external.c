#include <stdarg.h>

typedef int (*callback)(int);
extern int native_check(float, unsigned, ...);
extern int native_seven(long, long, long, long, long, long, long, ...);
extern int native_eight(long, long, long, long, long, long, long, long, ...);
extern double native_stack(long, long, long, long, long, long, long, long, float, double, ...);
extern int native_pull(va_list *);
extern int native_calls_clang(float, signed char, unsigned short, _Bool);
extern int native_calls_self(void);

long disturb(long a, long b, long c, long d, long e, long f, long g, long h, long i)
{
    return a + b + c + d + e + f + g + h + i;
}

int clang_pull(va_list *ap) { return va_arg(*ap, int); }

int clang_value(va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int i = va_arg(copy, int);
    va_end(copy);
    return i;
}

/* Compare exact bits without depending on Clang's FP comparison helpers. */
static unsigned long bits(double x)
{
    volatile union { double f; unsigned long u; } value = {.f = x};
    return value.u;
}

static unsigned bits32(float x)
{
    volatile union { float f; unsigned u; } value = {.f = x};
    return value.u;
}

int clang_check(float named, unsigned tag, ...)
{
    va_list ap, copy;
    va_start(ap, tag);
    int ok = bits32(named) == 0xbf000000U && tag == 4000000000U;
    ok &= native_pull(&ap) == -11;
    ok &= va_arg(ap, int) == 1;
    ok &= va_arg(ap, unsigned) == 4100000000U;
    ok &= va_arg(ap, long) == -0x100000007L;
    ok &= bits(va_arg(ap, double)) == 0xc00a000000000000UL;
    va_copy(copy, ap);
    ok &= bits(va_arg(ap, double)) == 0x401a000000000000UL;
    ok &= va_arg(ap, int) == 50000;
    ok &= *va_arg(ap, const int *) == 31;
    ok &= va_arg(ap, callback)(12) == 17;
    va_end(ap);
    ok &= bits(va_arg(copy, double)) == 0x401a000000000000UL && va_arg(copy, int) == 50000;
    va_end(copy);
    return ok ? 0 : 1;
}

static int next(int x) { return x + 5; }

int test_main(void)
{
    const int x = 31;
    if (native_check(-0.5f, 4000000000U, (signed char)-11, (_Bool)1, 4100000000U,
                     -0x100000007L, -3.25f, 6.5, (short)-48, &x, next))
        return 1;
    if (!native_seven(1, 2, 3, 4, 5, 6, 7, -90, 7.25))
        return 2;
    if (!native_eight(1, 2, 3, 4, 5, 6, 7, 8, -90))
        return 3;
    if (bits(native_stack(1, 2, 3, 4, 5, 6, 7, 8, 10.0f, -9.0, -6, -3.25)) !=
        0xc00a000000000000UL)
        return 4;
    if (native_calls_clang(-3.25f, -11, 50000, 1))
        return 5;
    if (native_calls_self() != -27)
        return 6;
    return 0;
}
