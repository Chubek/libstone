#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !defined(__CEPHYR__) || (defined(__CEPHYR_HAS_SCALAR_FP) && \
                             defined(__CEPHYR_HAS_VARIADIC_CALLS))
static double unpack(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

#ifndef __CEPHYR__
static int check_vsnprintf(const char *expected, const char *format, ...)
{
    char text[1100];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    return count == (int)strlen(expected) && strcmp(text, expected) == 0;
}
#endif

static char format_text[1100];
#define check(expected, ...) \
    (snprintf(format_text, sizeof format_text, __VA_ARGS__) == (int)strlen(expected) && \
     strcmp(format_text, expected) == 0)

#if !defined(__CEPHYR__) && (defined(__x86_64__) || defined(__aarch64__))
static int check_rounding(void)
{
    int ok = 1;
#if defined(__x86_64__)
    unsigned original, mode;
    __asm__ volatile ("stmxcsr %0" : "=m"(original));
    mode = original & ~(3U << 13);
#define SET_MODE(up, down) \
    do { \
        unsigned csr = mode | ((up ? 2U : down ? 1U : 3U) << 13); \
        __asm__ volatile ("ldmxcsr %0" : : "m"(csr)); \
    } while (0)
#else
    uint64_t original, mode;
    __asm__ volatile ("mrs %0, fpcr" : "=r"(original));
    mode = original & ~(UINT64_C(3) << 22);
#define SET_MODE(up, down) \
    do { \
        uint64_t fpcr = mode | ((up ? UINT64_C(1) : down ? UINT64_C(2) : \
                                 UINT64_C(3)) << 22); \
        __asm__ volatile ("msr fpcr, %0" : : "r"(fpcr)); \
    } while (0)
#endif
    SET_MODE(1, 0);
    ok &= check("1.3 -1.2 0x1.1p+0", "%.1f %.1f %.1a", 1.25, -1.25, 1.00390625);
    ok &= check("0.001 -0.000", "%.3f %.3f", 0x1p-1074, -0x1p-1074);
    SET_MODE(0, 1);
    ok &= check("1.2 -1.3 -0x1.1p+0", "%.1f %.1f %.1a", 1.25, -1.25, -1.00390625);
    ok &= check("0.000 -0.001", "%.3f %.3f", 0x1p-1074, -0x1p-1074);
    SET_MODE(0, 0);
    ok &= check("1 -1 -0x1p+0", "%.0f %.0f %.0a", 1.75, -1.75, -1.75);
#undef SET_MODE
#if defined(__x86_64__)
    __asm__ volatile ("ldmxcsr %0" : : "m"(original));
#else
    __asm__ volatile ("msr fpcr, %0" : : "r"(original));
#endif
    return ok;
}
#endif
#endif

int main(void)
{
#if !defined(__CEPHYR__) || (defined(__CEPHYR_HAS_SCALAR_FP) && \
                             defined(__CEPHYR_HAS_VARIADIC_CALLS))
    float float_argument = 1.5f;
    char text[16];
    int read_count, count = -1;
    if (setlocale(LC_ALL, "C") == NULL ||
        !check("-3: 0x1.8p+0 / 12.50 / 19", "%d: %la / %.2lf / %d",
               -3, float_argument, 12.5, 19) ||
        !check("1 2 3 4 5 6 7 8 1.5 19", "%g %g %g %g %g %g %g %g %g %d",
               1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, float_argument, 19) ||
        !check("0x0p+0|-0X0.P+0", "%a|%#A", 0.0, -0.0) ||
        !check("0x0.0000000000001p-1022", "%a", 0x1p-1074) ||
        !check("0x1.fffffffffffffp+1023", "%a", 0x1.fffffffffffffp1023) ||
        !check("0x0.000000000000p-1022", "%.12a", 0x1p-1074) ||
        !check("0x0.0000000000000p+0", "%.13a", 0.0))
        return 1;

    if (!check("[+0X000001.8P+0][ 0x1.p+0   ]", "[%+014.1A][%# 0*.*a]",
               1.5, -11, 0, 1.0) ||
        !check("0x1.0p+0 0x1.2p+0 0x2p+0", "%.1a %.1a %.0a",
               0x1.08p0, 0x1.18p0, 1.75) ||
        !check("  0x1.8p+0", "%10.*a", -1, 1.5))
        return 2;

    if (!check("     +INF| nan  |-INF|-NAN", "%+#09F|% -6a|%E|%G",
               unpack(0x7ff0000000000000UL), unpack(0x7ff8000000000001UL),
               unpack(0xfff0000000000000UL), unpack(0xfff8000000000001UL)) ||
        !check("2 4 -2 -0 1. 10.0", "%.0f %.0f %.0f %.0F %#.0f %.1f",
               2.5, 3.5, -2.5, -0.125, 1.0, 9.96875) ||
        !check("+00001.250000|1.E+00|-0.0e+00", "%+013f|%#.0E|%.1e",
               1.25, 1.0, -0.0))
        return 3;

    if (!check("1e+01|0.0001|1e-05|0.0001", "%.0g|%.1g|%.1g|%.1g",
               9.75, 0.0001, 0.00001, 0.00009999) ||
        !check("100|100.|1.000E+04|0.000", "%.3g|%#.3g|%#.4G|%#.4g",
               100.0, 100.0, 10000.0, 0.0) ||
        !check("4.9406564584124654e-324|1.7976931348623157E+308", "%.16e|%.16E",
               0x1p-1074, 0x1.fffffffffffffp1023) ||
        !check("0.1000000000000000055511151231257827021181583404541015625", "%.55f", 0.1))
        return 4;

    errno = EINVAL;
    if (snprintf(text, 5, "%.*f%n!", 4095, 0.125, &count) != 4098 ||
        count != 4097 || strcmp(text, "0.12") != 0 || errno != EINVAL ||
        snprintf(NULL, 0, "%.*f", INT_MAX - 2, 0.0) != INT_MAX ||
        snprintf(NULL, 0, "%.*g", INT_MAX, 1.0) != 1 ||
        snprintf(NULL, 0, "%.*a", INT_MAX - 7, 1.0) != INT_MAX ||
        snprintf(text, 1, "%1.1f", 100.0) != 5 || text[0] != '\0')
        return 5;
    errno = 0;
    if (snprintf(text, sizeof text, "pre%.*e", INT_MAX, 1.0) >= 0 ||
        errno != EOVERFLOW || strcmp(text, "pre") != 0)
        return 6;
#ifndef __CEPHYR__
    if (!check_vsnprintf("0x1.8p+0|1.25", "%a|%.2f", 1.5, 1.25))
        return 7;
    errno = 0;
    if (snprintf(text, sizeof text, "pre%Lf", 1.0L) >= 0 ||
        errno != EINVAL || strcmp(text, "pre") != 0)
        return 7;
#endif

    FILE *stream = tmpfile();
    if (!stream || fprintf(stream, "%s: %#.0a", "out", 4.0) != 12 ||
        fseek(stream, 0, SEEK_SET) != 0)
        return 8;
    read_count = (int)fread(text, 1, sizeof(text), stream);
    if (read_count != 12 || memcmp(text, "out: 0x1.p+2", 12) != 0 || fclose(stream) != 0)
        return 9;
    if (fprintf(stdin, "%g", 1.0) >= 0 || !ferror(stdin))
        return 10;
#if !defined(__CEPHYR__) && (defined(__x86_64__) || defined(__aarch64__))
    if (!check_rounding())
        return 11;
#endif
#endif
    return 0;
}
