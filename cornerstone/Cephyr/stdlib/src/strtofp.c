#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef CEPHYR_LITERAL_PARSER
_Static_assert(__FLT_RADIX__ == 2 && __FLT_MANT_DIG__ == 24 &&
               __FLT_MAX_EXP__ == 128 && sizeof(float) == 4 &&
               __DBL_MANT_DIG__ == 53 && __DBL_MAX_EXP__ == 1024 &&
               sizeof(double) == 8, "expected binary32 and binary64");
#endif

typedef struct {
    uint32_t *data;
    unsigned used;
} bigint;

static void copy(bigint *dst, const bigint *src)
{
    dst->used = src->used;
    memcpy(dst->data, src->data, src->used * sizeof(*src->data));
}

static void append_digit(bigint *a, unsigned base, unsigned digit)
{
    uint64_t carry = digit;
    for (unsigned i = 0; i < a->used; ++i) {
        carry += (uint64_t)a->data[i] * base;
        a->data[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (carry)
        a->data[a->used++] = (uint32_t)carry;
}

static unsigned bit_length(const bigint *a)
{
    if (!a->used)
        return 0;
    return 32 * a->used - (unsigned)__builtin_clz(a->data[a->used - 1]);
}

static void shift_left(bigint *a, unsigned count)
{
    if (!a->used || !count)
        return;
    unsigned words = count / 32;
    unsigned bits = count % 32;
    uint32_t carry = bits ? a->data[a->used - 1] >> (32 - bits) : 0;
    if (carry)
        a->data[a->used + words] = carry;
    for (unsigned i = a->used; i != 0; --i) {
        uint32_t low = bits && i > 1 ? a->data[i - 2] >> (32 - bits) : 0;
        a->data[i - 1 + words] = (a->data[i - 1] << bits) | low;
    }
    for (unsigned i = 0; i < words; ++i)
        a->data[i] = 0;
    a->used += words + (carry != 0);
}

static void shift_right_one(bigint *a)
{
    uint32_t carry = 0;
    for (unsigned i = a->used; i != 0; --i) {
        uint32_t word = a->data[i - 1];
        a->data[i - 1] = (word >> 1) | carry;
        carry = word << 31;
    }
    if (a->used && !a->data[a->used - 1])
        --a->used;
}

static int compare(const bigint *a, const bigint *b)
{
    if (a->used != b->used)
        return a->used < b->used ? -1 : 1;
    for (unsigned i = a->used; i != 0; --i)
        if (a->data[i - 1] != b->data[i - 1])
            return a->data[i - 1] < b->data[i - 1] ? -1 : 1;
    return 0;
}

static void subtract(bigint *a, const bigint *b)
{
    uint64_t borrow = 0;
    for (unsigned i = 0; i < a->used; ++i) {
        uint64_t word = (uint64_t)a->data[i] - borrow;
        if (i < b->used)
            word -= b->data[i];
        a->data[i] = (uint32_t)word;
        borrow = word >> 63;
    }
    while (a->used && !a->data[a->used - 1])
        --a->used;
}

enum rounding { nearest, upward, downward, toward_zero };

static enum rounding current_rounding(int extended)
{
#if defined(CEPHYR_LITERAL_PARSER)
    (void)extended;
#elif defined(__x86_64__)
    unsigned direction;
    if (extended) {
        unsigned short control;
        __asm__ volatile ("fnstcw %0" : "=m"(control));
        direction = control >> 10;
    } else {
        unsigned csr;
        __asm__ volatile ("stmxcsr %0" : "=m"(csr));
        direction = csr >> 13;
    }
    switch (direction & 3) {
    case 1: return downward;
    case 2: return upward;
    case 3: return toward_zero;
    }
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile ("mrs %0, fpcr" : "=r"(fpcr));
    switch ((fpcr >> 22) & 3) {
    case 1: return upward;
    case 2: return downward;
    case 3: return toward_zero;
    }
#endif
    (void)extended;
    /* The bundled RV64IM profile has no floating-point control register. */
    return nearest;
}

/* Work with packed sign/exponent/fraction bits; the x87 explicit integer
   bit is supplied when materializing a long double. No wide division needed. */
typedef unsigned __int128 fpbits;
#define ONE ((fpbits)1)
typedef struct {
    int fraction, emin, emax;
    int max_order10, min_order10;
    unsigned decimal_digits, limbs;
} format;

/* Midpoints have at most (p+1) significant bits. For binary64 their
   terminating decimals need <800 digits (2^54 * 5^1075 < 10^800); for
   binary128, 2^114 * 5^16495 < 10^11600, which also covers x87.
   Thus a discarded nonzero suffix can break a tie but cannot cross a
   boundary in the interior of the retained interval. Positive binary
   exponents need at most 4933 integer digits even for binary128.

   For the larger workspace A starts below 10^11600; for negative decimal
   powers B <= 5^(5000+11599). For positive powers A <= 10^5001.
   Normalization and division add at most 114 bits to the larger bit length:
   both temporaries still fit in 1220 * 32 bits. */
static const format binary32 = {23, -126, 127, 39, -46, 800, 96};
static const format binary64 = {52, -1022, 1023, 309, -325, 800, 96};
static const format extended = {__LDBL_MANT_DIG__ - 1, -16382, 16383, 5000, -5000,
                                11600, 1220};

static fpbits infinity(const format *f)
{
    return (fpbits)(f->emax * 2 + 1) << f->fraction;
}

static fpbits overflow(const format *f, enum rounding mode, int negative)
{
    errno = ERANGE;
    if (mode == toward_zero || mode == (negative ? upward : downward))
        return infinity(f) - 1;
    return infinity(f);
}

static uint64_t tiny(int away_from_zero)
{
    errno = ERANGE;
    return (uint64_t)away_from_zero;
}

/* Return the rounded magnitude of a * base^power. */
static fpbits convert(bigint *a, int base, int power, int sticky,
                      const format *f, int negative)
{
    enum rounding mode = current_rounding(f == &extended);
    int away_from_zero = mode == (negative ? downward : upward);
    uint32_t denominator[f->limbs], scratch[f->limbs];
    bigint b = {denominator, 1};
    denominator[0] = 1;
    if (base == 10) {
        bigint *scaled = power < 0 ? &b : a;
        int count = power < 0 ? -power : power;
        while (count >= 13) {
            append_digit(scaled, 1220703125U, 0); /* 5^13 */
            count -= 13;
        }
        while (count--)
            append_digit(scaled, 5, 0);
    }
    /* base 16 supplies power in bits already; decimal contributes 2^power
       after putting the 5^power factor into A or B. */
    int log_ratio = (int)bit_length(a) - (int)bit_length(&b);
    bigint trial = {scratch, 0};
    int cmp;
    if (log_ratio >= 0) {
        copy(&trial, &b);
        shift_left(&trial, (unsigned)log_ratio);
        cmp = compare(a, &trial);
    } else {
        copy(&trial, a);
        shift_left(&trial, (unsigned)-log_ratio);
        cmp = compare(&trial, &b);
    }
    int exponent = power + log_ratio - (cmp < 0);
    int min_quantum = f->emin - f->fraction;
    if (exponent > f->emax)
        return overflow(f, mode, negative);
    if (exponent < min_quantum - 1)
        return tiny(away_from_zero);

    int quantum = (exponent < f->emin ? f->emin : exponent) - f->fraction;
    if (power >= quantum)
        shift_left(a, (unsigned)(power - quantum));
    else
        shift_left(&b, (unsigned)(quantum - power));
    /* Divide A by B, leaving the exact remainder in A. The quotient fits
       in at most fraction+1 bits before rounding. */
    int bit = (int)bit_length(a) - (int)bit_length(&b);
    fpbits significand = 0;
    if (bit >= 0) {
        copy(&trial, &b);
        shift_left(&trial, (unsigned)bit);
        for (; bit >= 0; --bit) {
            if (compare(a, &trial) >= 0) {
                subtract(a, &trial);
                significand |= ONE << bit;
            }
            shift_right_one(&trial);
        }
    }
    int inexact = a->used != 0 || sticky;
    if (mode == nearest) {
        shift_left(a, 1);
        cmp = compare(a, &b);
        if (cmp > 0 || (cmp == 0 && (sticky || (significand & 1))))
            ++significand;
    } else if (away_from_zero && inexact) {
        ++significand;
    }
    fpbits hidden = ONE << f->fraction;
    if (significand >= hidden * 2) {
        significand >>= 1;
        ++exponent;
    }
    if (exponent > f->emax)
        return overflow(f, mode, negative);
    if (significand < hidden) {
        if (inexact)
            errno = ERANGE;
        return significand;
    }
    if (exponent < f->emin)
        exponent = f->emin;
    return ((fpbits)(exponent - f->emin + 1) << f->fraction) |
           (significand - hidden);
}

typedef struct {
    const void *text;
    size_t pos;
    int wide;
} cursor;

/* Both supported locale names (C and POSIX) use the same ASCII grammar.
   Fold all non-ASCII values to a single nonmatching input character. */
static int peek(cursor c)
{
    unsigned value = c.wide ? (unsigned)((const wchar_t *)c.text)[c.pos]
                            : ((const unsigned char *)c.text)[c.pos];
    return value <= 127 ? (int)value : 128;
}

static int lower_ascii(int ch)
{
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}

static int accept(cursor *c, const char *word)
{
    cursor scan = *c;
    for (; *word; ++word, ++scan.pos)
        if (lower_ascii(peek(scan)) != *word)
            return 0;
    *c = scan;
    return 1;
}

static int digit_value(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    ch = lower_ascii(ch);
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    return 16;
}

/* Exponents may contain arbitrarily many digits. Saturation is far beyond
   the possible significand length in any supported Linux address space. */
#define EXP_LIMIT (LLONG_MAX / 16)

static long long scan_exponent(cursor *c, int marker)
{
    cursor scan = *c;
    if (lower_ascii(peek(scan)) != marker)
        return 0;
    ++scan.pos;
    int sign = peek(scan);
    if (sign == '-' || sign == '+')
        ++scan.pos;
    int digit = digit_value(peek(scan));
    if (digit >= 10)
        return 0;
    long long value = 0;
    do {
        if (value > (EXP_LIMIT - digit) / 10)
            value = EXP_LIMIT;
        else
            value = value * 10 + digit;
        ++scan.pos;
        digit = digit_value(peek(scan));
    } while (digit < 10);
    *c = scan;
    return sign == '-' ? -value : value;
}

static fpbits parse(const void *text, int wide, size_t *end, const format *f)
{
    cursor c = {text, 0, wide};
    *end = 0;
    int ch = peek(c);
    while (ch == ' ' || (ch >= '\t' && ch <= '\r')) {
        ++c.pos;
        ch = peek(c);
    }
    int negative = ch == '-';
    fpbits sign = negative ? (fpbits)(2 * f->emax + 2) << f->fraction : 0;
    if (negative || ch == '+')
        ++c.pos;
    if (accept(&c, "inf")) {
        accept(&c, "inity");
        *end = c.pos;
        return sign | infinity(f);
    }
    if (accept(&c, "nan")) {
        cursor scan = c;
        if (peek(scan) == '(') {
            do {
                ++scan.pos;
                ch = lower_ascii(peek(scan));
            } while ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_');
            if (ch == ')')
                c.pos = scan.pos + 1;
        }
        *end = c.pos;
        /* The optional payload is accepted, but does not change the NaN. */
        return sign | infinity(f) | (ONE << (f->fraction - 1));
    }

    int base = 10;
    cursor hex = c;
    if (accept(&hex, "0x")) {
        cursor first = hex;
        if (peek(first) == '.')
            ++first.pos;
        if (digit_value(peek(first)) < 16) {
            base = 16;
            c = hex;
        }
    }
    unsigned max_digits = base == 10 ? f->decimal_digits : (unsigned)f->fraction / 4 + 3;
    uint32_t numerator[f->limbs];
    bigint a = {numerator, 0};
    unsigned kept = 0;
    int seen_digit = 0, point = 0, sticky = 0;
    long long order = 0;
    for (;;) {
        ch = peek(c);
        if (ch == '.' && !point) {
            point = 1;
        } else {
            int digit = digit_value(ch);
            if (digit >= base)
                break;
            seen_digit = 1;
            /* order is the position of the first nonzero digit relative
               to the point: e.g. 10 has order 1 and .01 has order -2. */
            if (kept && !point)
                ++order;
            else if (!kept && point)
                --order;
            if (kept || digit) {
                if (kept < max_digits) {
                    append_digit(&a, (unsigned)base, (unsigned)digit);
                    ++kept;
                } else {
                    sticky |= digit != 0;
                }
            }
        }
        ++c.pos;
    }
    if (!seen_digit)
        return 0;

    long long power = scan_exponent(&c, base == 10 ? 'e' : 'p');
    *end = c.pos;
    if (!kept)
        return sign;
    int stride = base == 10 ? 1 : 4;
    order = order * stride + power;
    if (order > (base == 10 ? f->max_order10 : f->emax))
        return sign | overflow(f, current_rounding(f == &extended), negative);
    if (order < (base == 10 ? f->min_order10 : f->emin - f->fraction - 4))
        return sign | tiny(current_rounding(f == &extended) == (negative ? downward : upward));
    power = order - stride * (kept - 1);
    return sign | convert(&a, base, (int)power, sticky, f, negative);
}

#ifdef CEPHYR_LITERAL_PARSER
/* Compiler-facing entry: parse validated tokens directly to target bits,
   independent of the host FP formats, locale and runtime rounding mode. */
int cephyr_literal_bits(const char *text, unsigned width, uint64_t *low, uint64_t *high)
{
    static const format x87 = {63, -16382, 16383, 5000, -5000, 11600, 1220};
    static const format quad = {112, -16382, 16383, 5000, -5000, 11600, 1220};
    const format *f = width == 32 ? &binary32 : width == 64 ? &binary64 : width == 80 ? &x87 : &quad;
    size_t end;
    int saved_errno = errno;
    fpbits parsed = parse(text, 0, &end, f);
    errno = saved_errno;
    if (text[end] || parsed >= infinity(f)) return 0;
    if (width == 80) {
        *high = (uint64_t)(parsed >> 63);
        *low = (uint64_t)(parsed & ((ONE << 63) - 1)) | ((uint64_t)(*high != 0) << 63);
    } else {
        *high = (uint64_t)(parsed >> 64);
        *low = (uint64_t)parsed;
    }
    return 1;
}
#else
float strtof(const char *restrict text, char **restrict end)
{
    size_t pos;
    union { uint32_t bits; float value; } result;
    result.bits = (uint32_t)parse(text, 0, &pos, &binary32);
    if (end)
        *end = (char *)text + pos;
    return result.value;
}

double strtod(const char *restrict text, char **restrict end)
{
    size_t pos;
    union { uint64_t bits; double value; } result;
    result.bits = parse(text, 0, &pos, &binary64);
    if (end)
        *end = (char *)text + pos;
    return result.value;
}

double atof(const char *text)
{
    return strtod(text, NULL);
}

float wcstof(const wchar_t *restrict text, wchar_t **restrict end)
{
    size_t pos;
    union { uint32_t bits; float value; } result;
    result.bits = (uint32_t)parse(text, 1, &pos, &binary32);
    if (end)
        *end = (wchar_t *)text + pos;
    return result.value;
}

double wcstod(const wchar_t *restrict text, wchar_t **restrict end)
{
    size_t pos;
    union { uint64_t bits; double value; } result;
    result.bits = parse(text, 1, &pos, &binary64);
    if (end)
        *end = (wchar_t *)text + pos;
    return result.value;
}

static long double unpack_extended(fpbits bits)
{
    _Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ &&
                   sizeof(long double) == 16 && __LDBL_MAX_EXP__ == 16384,
                   "expected a bundled long double ABI");
    union { fpbits bits; long double value; } result;
#if defined(__x86_64__)
    _Static_assert(__LDBL_MANT_DIG__ == 64, "expected padded x87 extended");
    unsigned sign_exponent = (unsigned)(bits >> 63);
    result.bits = ((fpbits)sign_exponent << 64) | (bits & ((ONE << 63) - 1));
    if (sign_exponent & 0x7fff)
        result.bits |= ONE << 63;
#else
    _Static_assert(__LDBL_MANT_DIG__ == 113, "expected IEEE binary128");
    result.bits = bits;
#endif
    return result.value;
}

long double strtold(const char *restrict text, char **restrict end)
{
    size_t pos;
    fpbits bits = parse(text, 0, &pos, &extended);
    if (end)
        *end = (char *)text + pos;
    return unpack_extended(bits);
}

long double wcstold(const wchar_t *restrict text, wchar_t **restrict end)
{
    size_t pos;
    fpbits bits = parse(text, 1, &pos, &extended);
    if (end)
        *end = (wchar_t *)text + pos;
    return unpack_extended(bits);
}

#endif
