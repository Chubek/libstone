#include <errno.h>
#include <locale.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if !defined(__CEPHYR__) || defined(__CEPHYR_HAS_SCALAR_FP)

static uint64_t double_bits(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static int check_double(const char *text, uint64_t bits, int consumed, int error)
{
    char *end;
    errno = EINVAL;
    double value = strtod(text, &end);
    return double_bits(value) == bits && end == text + consumed && errno == error;
}

static int check_float(const char *text, uint32_t bits, int consumed, int error)
{
    char *end;
    errno = EINVAL;
    float value = strtof(text, &end);
    return float_bits(value) == bits && end == text + consumed && errno == error;
}

#if !defined(__CEPHYR__) && (defined(__x86_64__) || defined(__aarch64__))
/* Set the hardware mode independently of fenv.h to test strtofp itself. */
static void set_rounding(unsigned mode)
{
#if defined(__x86_64__)
    unsigned csr;
    __asm__ volatile ("stmxcsr %0" : "=m"(csr));
    csr = (csr & ~(3U << 13)) | (mode << 13);
    __asm__ volatile ("ldmxcsr %0" : : "m"(csr));
#else
    uint64_t fpcr;
    __asm__ volatile ("mrs %0, fpcr" : "=r"(fpcr));
    if (mode == 1 || mode == 2)
        mode ^= 3;
    fpcr = (fpcr & ~(3UL << 22)) | ((uint64_t)mode << 22);
    __asm__ volatile ("msr fpcr, %0" : : "r"(fpcr));
#endif
}

static int check_rounding(void)
{
    set_rounding(1);
    int down = check_double("1.0000000000000001", 0x3ff0000000000000UL, 18, EINVAL) &&
        check_double("-1.0000000000000001", 0xbff0000000000001UL, 19, EINVAL) &&
        check_float("1e90", 0x7f7fffffU, 4, ERANGE) &&
        check_float("-1e90", 0xff800000U, 5, ERANGE);
    set_rounding(2);
    int up = check_double("0x1.00000000000001", 0x3ff0000000000001UL, 18, EINVAL) &&
        check_double("-0x1.00000000000001", 0xbff0000000000000UL, 19, EINVAL) &&
        check_float("1e-90", 1, 5, ERANGE) &&
        check_float("-1e-90", 0x80000000U, 6, ERANGE);
    set_rounding(3);
    int zero = check_float("0x1.ffffffp127", 0x7f7fffffU, 14, EINVAL) &&
        check_float("-1e90", 0xff7fffffU, 5, ERANGE);
    set_rounding(0);
    return down && up && zero;
}
#endif

int main(void)
{
    if (!check_double(" \t\n\v\f\r-12.5e+1#", 0xc05f400000000000UL, 14, EINVAL) ||
        !check_double("  +.e-3", 0, 0, EINVAL) ||
        !check_double("0x.p1", 0, 1, EINVAL) ||
        !check_double("42e+", 0x4045000000000000UL, 2, EINVAL) ||
        !check_double("0X.8P-!", 0x3fe0000000000000UL, 4, EINVAL) ||
        !check_double("0x0.8", 0x3fe0000000000000UL, 5, EINVAL))
        return 1;

    if (!check_double("-InFiNiTy!", 0xfff0000000000000UL, 9, EINVAL) ||
        !check_double("+infinite", 0x7ff0000000000000UL, 4, EINVAL) ||
        !check_double("-NaN(_A19)", 0xfff8000000000000UL, 10, EINVAL) ||
        !check_double("NaN(a-b)", 0x7ff8000000000000UL, 3, EINVAL) ||
        !check_float("nAn()", 0x7fc00000U, 5, EINVAL) ||
        !check_float(" -0e999999999999999999999999999", 0x80000000U, 31, EINVAL))
        return 2;

    if (!check_float("1.000000059604644775390625", 0x3f800000U, 26, EINVAL) ||
        !check_float("1.00000005960464477539062500000001", 0x3f800001U, 34, EINVAL) ||
        !check_float("0x1.000003p0", 0x3f800002U, 12, EINVAL) ||
        !check_double("0x1.00000000000008p0", 0x3ff0000000000000UL, 20, EINVAL) ||
        !check_double("0x1.00000000000008000001p0", 0x3ff0000000000001UL, 26, EINVAL))
        return 3;

    if (!check_double("0x1.fffffffffffffp1023", 0x7fefffffffffffffUL, 22, EINVAL) ||
        !check_double("0x1.fffffffffffff8p1023", 0x7ff0000000000000UL, 23, ERANGE) ||
        !check_double("0x1p-1074", 1, 9, EINVAL) ||
        !check_double("-0x1p-1075", 0x8000000000000000UL, 10, ERANGE) ||
        !check_double("0x1.0000000000000000001p-1075", 1, 29, ERANGE) ||
        !check_double("2.2250738585072014e-308", 0x0010000000000000UL, 23, EINVAL) ||
        !check_float("0x1p-149", 1, 8, EINVAL) ||
        !check_float("1e-40", 0x000116c2U, 5, ERANGE) ||
        !check_float("1e+99999999999999999999999!", 0x7f800000U, 26, ERANGE))
        return 4;

    /* A late digit well beyond the retained prefix breaks a halfway tie. */
    char long_text[1024];
    const char *tie = "1.000000059604644775390625";
    strcpy(long_text, tie);
    memset(long_text + 26, '0', 990 - 26);
    long_text[990] = '1';
    long_text[991] = '\0';
    if (!check_float(long_text, 0x3f800001U, 991, EINVAL))
        return 5;
    memset(long_text, '0', 1000);
    long_text[0] = '1';
    strcpy(long_text + 1000, "e-999!");
    if (!check_double(long_text, 0x3ff0000000000000UL, 1005, EINVAL))
        return 6;

    if (setlocale(LC_NUMERIC, "POSIX") == NULL ||
        !check_double("1,5", 0x3ff0000000000000UL, 1, EINVAL) ||
        double_bits(atof(" .25?")) != 0x3fd0000000000000UL ||
        double_bits(strtod("123.", NULL)) != 0x405ec00000000000UL)
        return 7;

    wchar_t *end;
    const wchar_t *wide = L" \t-0X1.4P+2,";
    if (double_bits(wcstod(wide, &end)) != 0xc014000000000000UL || *end != ',')
        return 8;
    const wchar_t stop[] = {'1', '.', '5', 0x100, 0};
    if (float_bits(wcstof(stop, &end)) != 0x3fc00000U || end != stop + 3)
        return 9;
    const wchar_t *bad = L" +.";
    if (float_bits(wcstof(bad, &end)) != 0 || end != bad ||
        double_bits(wcstod(L"+NaN(x_1)", NULL)) != 0x7ff8000000000000UL)
        return 10;

#if !defined(__CEPHYR__) && (defined(__x86_64__) || defined(__aarch64__))
    if (!check_rounding())
        return 11;
#endif
    return 0;
}

#else

/* These declarations conflict if RV64IM Cephyr sees FP-return prototypes. */
extern void strtof(void), strtod(void), atof(void), wcstof(void), wcstod(void);
int main(void) { return 0; }

#endif
