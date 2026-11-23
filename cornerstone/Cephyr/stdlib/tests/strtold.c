#include <assert.h>
#include <errno.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#if defined(__x86_64__) || defined(__aarch64__)
#include <fenv.h>
#endif

/* Compare the ABI's value bytes, not x87 padding. No target FP arithmetic
   or comparison helpers are needed, even in the RV64IM soft-float image. */
#if LDBL_MANT_DIG == 64
#define VALUE_BYTES 10
#define HALF "1.0000000000000000000542101086242752217003726400434970855712890625"
#define NEXT 0x1.0000000000000002p0L
#define MIN_HEX "0x1p-16445"
#define HALF_MIN "-0x1p-16446"
#define MIN_DEC "3.6451995318824746025284059336194e-4951"
#define MAX_HEX "0x1.fffffffffffffffep16383"
#define OVER_TIE "0x1.ffffffffffffffffp16383"
#define NORMAL_TIE "0x0.ffffffffffffffffp-16382"
#else
#define VALUE_BYTES 16
#define HALF "1.00000000000000000000000000000000009629649721936179265279889712924636592690508241076940976199693977832794189453125"
#define NEXT 0x1.0000000000000000000000000001p0L
#define MIN_HEX "0x1p-16494"
#define HALF_MIN "-0x1p-16495"
#define MIN_DEC "6.4751751194380251109244389582276e-4966"
#define MAX_HEX "0x1.ffffffffffffffffffffffffffffp16383"
#define OVER_TIE "0x1.ffffffffffffffffffffffffffff8p16383"
#define NORMAL_TIE "0x0.ffffffffffffffffffffffffffff8p-16382"
#endif

static void check(const char *text, size_t consumed, long double expect, int error)
{
    char *end;
    errno = EINVAL;
    long double x = strtold(text, &end);
    assert(end == text + consumed && errno == error);
    assert(memcmp(&x, &expect, VALUE_BYTES) == 0);

    /* A wide non-ASCII trailing character with a digit in its low byte
       verifies that wcstold neither truncates the input nor misplaces end. */
    static wchar_t wide[12004];
    size_t len = strlen(text);
    assert(len + 2 <= sizeof(wide) / sizeof(*wide));
    for (size_t i = 0; i < len; ++i)
        wide[i] = (unsigned char)text[i];
    wide[len] = 0x131;
    wide[len + 1] = 0;
    wchar_t *wend;
    errno = EINVAL;
    x = wcstold(wide, &wend);
    assert(wend == wide + consumed && errno == error);
    assert(memcmp(&x, &expect, VALUE_BYTES) == 0);
}

#define FULL(text, expect, error) check(text, sizeof(text) - 1, expect, error)

static void least_midpoint(void)
{
    /* 2^-k = 5^k * 10^-k. Generate its entire decimal expansion, then
       change only the last digit to land on either side of the midpoint.
       These inputs agree for over 11,000 significant decimal digits. */
    const unsigned k = LDBL_MANT_DIG + 16382;
    static char buffer[12000];
    char *suffix = buffer + sizeof(buffer) - 8;
    char *first = suffix - 1;
    *first = '1';
    for (unsigned left = k; left;) {
        unsigned step = left >= 8 ? 8 : 1;
        unsigned factor = step == 8 ? 390625 : 5;
        unsigned carry = 0;
        for (char *p = suffix; p != first;) {
            --p;
            carry += (unsigned)(*p - '0') * factor;
            *p = (char)('0' + carry % 10);
            carry /= 10;
        }
        while (carry) {
            *--first = (char)('0' + carry % 10);
            carry /= 10;
        }
        left -= step;
    }
    strcpy(suffix, k == 16446 ? "e-16446" : "e-16495");
    size_t len = strlen(first);
    check(first, len, 0.L, ERANGE);
    --suffix[-1];
    check(first, len, 0.L, ERANGE);
    suffix[-1] += 2;
    check(first, len, LDBL_TRUE_MIN, ERANGE);
}

int main(void)
{
    check("  -.e", 0, 0.L, EINVAL);
    check("0x.p2", 1, 0.L, EINVAL);
    check(" \t-42e+!", 5, -42.L, EINVAL);
    check(" \t-0x1p+!", 6, -1.L, EINVAL);
    FULL("-0e99999999999999999999", -0.L, EINVAL);
    FULL("-iNFiniTY", -__builtin_infl(), EINVAL);
    FULL("-nan(payload_1)", -__builtin_nanl(""), EINVAL);
    check("NaN(bad-)", 3, __builtin_nanl(""), EINVAL);
    FULL("0x1p12000", 0x1p12000L, EINVAL);
    FULL("1e4000", 1e4000L, EINVAL);
    FULL("1e-4000", 1e-4000L, EINVAL);
    FULL(MAX_HEX, LDBL_MAX, EINVAL);
    FULL(OVER_TIE, __builtin_infl(), ERANGE);
    FULL("0x1p-16382", LDBL_MIN, EINVAL);
    FULL(NORMAL_TIE, LDBL_MIN, EINVAL);
    FULL(MIN_HEX, LDBL_TRUE_MIN, EINVAL);
    FULL(MIN_DEC, LDBL_TRUE_MIN, ERANGE);
    FULL(HALF_MIN, -0.L, ERANGE);
    FULL(HALF, 1.L, EINVAL);
    static char sticky[12002] = HALF;
    memset(sticky + sizeof(HALF) - 1, '0', 12000 - (sizeof(HALF) - 1));
    sticky[12000] = '1';
    check(sticky, 12001, NEXT, EINVAL);
    least_midpoint();
#if defined(__x86_64__) || defined(__aarch64__)
    assert(fesetround(FE_UPWARD) == 0);
    FULL(HALF, NEXT, EINVAL);
    FULL("1e-99999", LDBL_TRUE_MIN, ERANGE);
    assert(fesetround(FE_DOWNWARD) == 0);
    FULL("-" HALF, -NEXT, EINVAL);
    FULL("1e99999", LDBL_MAX, ERANGE);
    assert(fesetround(FE_TOWARDZERO) == 0);
    FULL("-1e99999", -LDBL_MAX, ERANGE);
    assert(fesetround(FE_TONEAREST) == 0);
#endif
    return 0;
}
