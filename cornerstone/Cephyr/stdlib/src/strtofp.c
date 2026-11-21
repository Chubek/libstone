#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

_Static_assert(__FLT_RADIX__ == 2 && __FLT_MANT_DIG__ == 24 &&
               __FLT_MAX_EXP__ == 128 && sizeof(float) == 4 &&
               __DBL_MANT_DIG__ == 53 && __DBL_MAX_EXP__ == 1024 &&
               sizeof(double) == 8, "expected binary32 and binary64");

/* Every binary64 rounding boundary (including midpoints) is a dyadic with
   at most 54 significant bits and denominator at most 2^1075. Its terminating
   decimal has at most 768 significant digits: 2^54 * 5^1075 < 10^768.
   Retaining 800 digits puts any boundary outside the discarded interval,
   except possibly at its lower endpoint, where a sticky bit resolves it.
   Sixteen significant hex digits likewise suffice for the dyadic boundaries. */
enum { decimal_digits = 800, hex_digits = 16, limbs = 96 };

typedef struct {
    uint32_t data[limbs];
    unsigned used;
} bigint;

/* Only used with small multipliers (at most 16). */
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

static enum rounding current_rounding(void)
{
#if defined(__x86_64__)
    unsigned csr;
    __asm__ volatile ("stmxcsr %0" : "=m"(csr));
    switch ((csr >> 13) & 3) {
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
    /* The bundled RV64IM profile has no floating-point control register. */
    return nearest;
}

typedef struct {
    int fraction, emin, emax;
    uint64_t infinity, sign;
} format;

static const format binary32 = {23, -126, 127, UINT32_C(0x7f800000), UINT32_C(0x80000000)};
static const format binary64 = {52, -1022, 1023, UINT64_C(0x7ff0000000000000),
                                 UINT64_C(0x8000000000000000)};

static uint64_t overflow(const format *f, enum rounding mode, int negative)
{
    errno = ERANGE;
    if (mode == toward_zero || mode == (negative ? upward : downward))
        return f->infinity - 1;
    return f->infinity;
}

static uint64_t tiny(int away_from_zero)
{
    errno = ERANGE;
    return (uint64_t)away_from_zero;
}

/* Return the rounded magnitude of a * base^power. The caller has already
   bounded the scientific exponent, so -1124 <= power <= 309 for decimal.
   A fits initially in 2658 bits (or after positive powers of five well under
   that, since decimal digit count + power <= 310); B fits in 2610 bits.
   Normalization and division need at most 54 extra bits beyond these bounds. */
static uint64_t convert(bigint *a, int base, int power, int sticky,
                        const format *f, int negative)
{
    enum rounding mode = current_rounding();
    int away_from_zero = mode == (negative ? downward : upward);
    bigint b = {{1}, 1};
    if (base == 10) {
        bigint *scaled = power < 0 ? &b : a;
        int count = power < 0 ? -power : power;
        while (count--)
            append_digit(scaled, 5, 0);
    }
    /* base 16 supplies power in bits already; decimal contributes 2^power
       after putting the 5^power factor into A or B. */
    int log_ratio = (int)bit_length(a) - (int)bit_length(&b);
    bigint trial;
    int cmp;
    if (log_ratio >= 0) {
        trial = b;
        shift_left(&trial, (unsigned)log_ratio);
        cmp = compare(a, &trial);
    } else {
        trial = *a;
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
    uint64_t significand = 0;
    if (bit >= 0) {
        trial = b;
        shift_left(&trial, (unsigned)bit);
        for (; bit >= 0; --bit) {
            if (compare(a, &trial) >= 0) {
                subtract(a, &trial);
                significand |= UINT64_C(1) << bit;
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
    uint64_t hidden = UINT64_C(1) << f->fraction;
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
    return ((uint64_t)(exponent - f->emin + 1) << f->fraction) |
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

static uint64_t parse(const void *text, int wide, size_t *end, const format *f)
{
    cursor c = {text, 0, wide};
    *end = 0;
    int ch = peek(c);
    while (ch == ' ' || (ch >= '\t' && ch <= '\r')) {
        ++c.pos;
        ch = peek(c);
    }
    int negative = ch == '-';
    uint64_t sign = negative ? f->sign : 0;
    if (negative || ch == '+')
        ++c.pos;
    if (accept(&c, "inf")) {
        accept(&c, "inity");
        *end = c.pos;
        return sign | f->infinity;
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
        return sign | f->infinity | (UINT64_C(1) << (f->fraction - 1));
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
    unsigned max_digits = base == 10 ? decimal_digits : hex_digits;
    bigint a = {{0}, 0};
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
    if (order > (base == 10 ? 309 : 1024))
        return sign | overflow(f, current_rounding(), negative);
    if (order < (base == 10 ? -325 : -1078))
        return sign | tiny(current_rounding() == (negative ? downward : upward));
    power = order - stride * (kept - 1);
    return sign | convert(&a, base, (int)power, sticky, f, negative);
}

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
