#include <stdarg.h>

#ifndef __CEPHYR_HAS_RV64_STDARG
#error "bundled RV64IM stdarg was not enabled"
#endif
#ifdef __CEPHYR_HAS_SCALAR_FP
#error "RV64IM does not use the hardware floating-point calling convention"
#endif
_Static_assert(sizeof(va_list) == sizeof(void *), "RV64IM va_list is one pointer");

typedef int (*callback)(int);
extern long disturb(long, long, long, long, long, long, long, long, long);
extern int clang_pull(va_list *);
extern int clang_check(float, unsigned, ...);
extern int clang_value(va_list);

int native_pull(va_list *ap)
{
    return va_arg(*ap, int);
}

int native_check(float named, unsigned tag, ...)
{
    va_list lists[2], copy;
    int index = 0;
    /* Deliberately overwrite the incoming argument registers before va_start. */
    if (disturb(1, 2, 3, 4, 5, 6, 7, 8, 9) != 45 ||
        named != -0.5f || tag != 4000000000U)
        return 1;
    tag = 0; /* The value of parmN must not be read by va_start. */
    va_start(lists[index++], tag);
    int ok = index == 1 && sizeof va_arg(lists[0], long) == sizeof(long) &&
             _Generic(va_arg(lists[0], double), double: 1, default: 0);
    index = 0;
    ok &= va_arg(lists[index++], int) == -11 && index == 1; /* a2 */
    ok &= clang_value(lists[0]) == 1;                      /* a3, by-value copy */
    ok &= va_arg(lists[0], int) == 1;                       /* a3 */
    ok &= va_arg(lists[0], unsigned) == 4100000000U;        /* a4 */
    ok &= va_arg(lists[0], long) == -0x100000007L;           /* a5 */
    ok &= va_arg(lists[0], double) == -3.25;                 /* a6 */
    va_copy(copy, lists[0]);
    ok &= va_arg(lists[0], double) == 6.5;                   /* a7 */
    ok &= clang_pull(&lists[0]) == -48;                     /* entry sp + 0 */
    ok &= *va_arg(lists[0], const int *) == 31;              /* entry sp + 8 */
    ok &= va_arg(lists[0], callback)(12) == 17;              /* entry sp + 16 */
    va_end(lists[0]);
    ok &= va_arg(copy, double) == 6.5 && va_arg(copy, int) == -48;
    va_end(copy);
    va_start(lists[0], tag);
    ok &= va_arg(lists[0], int) == -11;
    va_end(lists[0]);
    return ok ? 0 : 2;
}

int native_seven(long a, long b, long c, long d, long e, long f, long g, ...)
{
    /* Make the save slot and cursor exceed a signed 12-bit frame offset. */
    unsigned char scratch[4097];
    scratch[4096] = 17;
    va_list ap;
    va_start(ap, g); /* An odd-sized save area must have no padding after a7. */
    int i = va_arg(ap, int);
    double j = va_arg(ap, double);
    va_end(ap);
    return scratch[4096] == 17 && a + b + c + d + e + f + g == 28 && i == -90 && j == 7.25;
}

int native_eight(long a, long b, long c, long d, long e, long f, long g, long h, ...)
{
    va_list ap;
    va_start(ap, h);
    int i = va_arg(ap, int);
    va_end(ap);
    return a + b + c + d + e + f + g + h == 36 && i == -90;
}

double native_stack(long a, long b, long c, long d, long e, long f, long g,
                    long h, float i, double last, ...)
{
    va_list ap;
    va_start(ap, last); /* Ignore two named stack slots before starting. */
    int n = va_arg(ap, int);
    double result = va_arg(ap, double);
    va_end(ap);
    return a + b + c + d + e + f + g + h == 36 && i == 10.0f &&
           last == -9.0 && n == -6 ? result : 0.0;
}

static int next(int x) { return x + 5; }

int native_calls_clang(float fp, signed char small, unsigned short narrow, _Bool truth)
{
    int x = 31;
    const int *p = &x;
    return clang_check(-0.5f, 4000000000U, small, truth, 4100000000U,
                       -0x100000007L, fp, 6.5, narrow, p, next);
}

static int self(int ignore, ...)
{
    register va_list ap, copy;
    va_start(ap, ignore);
    va_copy(copy, ap);
    int i = va_arg(copy, int);
    va_end(copy);
    va_end(ap);
    return i;
}

int native_calls_self(void) { return self(0, (signed char)-27); }
