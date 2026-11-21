#ifndef CEPHYR_PRINTF_FLOAT_H
#define CEPHYR_PRINTF_FLOAT_H

#include <stdint.h>
#include <string.h>

_Static_assert(__FLT_RADIX__ == 2 && __DBL_MANT_DIG__ == 53 &&
                   __DBL_MIN_EXP__ == -1021 && __DBL_MAX_EXP__ == 1024 &&
                   sizeof(double) == sizeof(uint64_t),
               "printf requires binary64 double");

/* The digits represent an unsigned significand, with `point` digits before
   the radix point. Missing digits on either end are implicitly zero. This
   keeps storage bounded by binary64, not by the user-supplied precision. */
enum { fp_digits = 768 };
struct fp_output {
    char digits[fp_digits];
    int length;
    int point;
    size_t fraction;
    char exponent[6];
    size_t exponent_length;
    char sign;
    int hex;
    int special;
    int uppercase;
};

enum fp_rounding { fp_nearest, fp_upward, fp_downward, fp_toward_zero };

static enum fp_rounding fp_current_rounding(void)
{
#if defined(__x86_64__)
    unsigned csr;
    __asm__ volatile ("stmxcsr %0" : "=m"(csr));
    switch ((csr >> 13) & 3) {
    case 1: return fp_downward;
    case 2: return fp_upward;
    case 3: return fp_toward_zero;
    }
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile ("mrs %0, fpcr" : "=r"(fpcr));
    switch ((fpcr >> 22) & 3) {
    case 1: return fp_upward;
    case 2: return fp_downward;
    case 3: return fp_toward_zero;
    }
#endif
    /* The bundled RV64IM profile has no floating-point control register. */
    return fp_nearest;
}

/* `half` compares the discarded magnitude against half an output unit. */
static int fp_round_up(char sign, int half, int odd)
{
    switch (fp_current_rounding()) {
    case fp_upward: return sign != '-';
    case fp_downward: return sign == '-';
    case fp_toward_zero: return 0;
    default: return half > 0 || (half == 0 && odd);
    }
}

static void fp_exponent(struct fp_output *out, int exponent, char marker,
                        int minimum_digits)
{
    char reversed[4];
    int digits = 0;
    unsigned value = exponent < 0 ? (unsigned)-exponent : (unsigned)exponent;
    do {
        reversed[digits++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 || digits < minimum_digits);
    out->exponent[0] = marker;
    out->exponent[1] = exponent < 0 ? '-' : '+';
    for (int i = 0; i < digits; ++i)
        out->exponent[2 + i] = reversed[digits - 1 - i];
    out->exponent_length = 2 + (size_t)digits;
}

static void fp_trim(struct fp_output *out)
{
    while (out->length > 1 && out->digits[out->length - 1] == '0')
        --out->length;
}

static void fp_hex(struct fp_output *out, uint64_t mantissa, int exponent,
                   int precision)
{
    const char *hex = out->uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    if (precision >= 0 && precision < 13) {
        unsigned shift = 52 - 4 * (unsigned)precision;
        uint64_t unit = UINT64_C(1) << shift;
        uint64_t remainder = mantissa & (unit - 1);
        uint64_t halfway = unit >> 1;
        mantissa -= remainder;
        if (remainder != 0 &&
            fp_round_up(out->sign, (remainder > halfway) - (remainder < halfway),
                        (int)((mantissa >> shift) & 1)))
            mantissa += unit;
    }
    for (unsigned i = 0; i <= 13; ++i)
        out->digits[i] = hex[(mantissa >> (52 - 4 * i)) & 15];
    out->length = 14;
    fp_trim(out);
    out->point = 1;
    out->fraction = precision < 0 ? (size_t)(out->length - 1)
                                  : (size_t)precision;
    out->hex = 1;
    fp_exponent(out, exponent, out->uppercase ? 'P' : 'p', 1);
}

#define CEPHYR_PRINTF_LIMB_BASE UINT32_C(1000000000)

static void fp_multiply(uint32_t *limbs, unsigned *used, uint32_t factor)
{
    uint64_t carry = 0;
    for (unsigned i = 0; i < *used; ++i) {
        uint64_t product = (uint64_t)limbs[i] * factor + carry;
        limbs[i] = (uint32_t)(product % CEPHYR_PRINTF_LIMB_BASE);
        carry = product / CEPHYR_PRINTF_LIMB_BASE;
    }
    if (carry != 0)
        limbs[(*used)++] = (uint32_t)carry;
}

static void fp_decimal_exact(struct fp_output *out, uint64_t mantissa, int power)
{
    if (mantissa == 0) {
        out->digits[0] = '0';
        out->length = out->point = 1;
        return;
    }

    /* m * 2^(-k) = m * 5^k / 10^k; for binary64 the numerator is less
       than 2^53 * 5^1074 < 10^768. Integers need at most 309 digits. */
    uint32_t limbs[(fp_digits + 8) / 9];
    unsigned used = 0;
    do {
        limbs[used++] = (uint32_t)(mantissa % CEPHYR_PRINTF_LIMB_BASE);
        mantissa /= CEPHYR_PRINTF_LIMB_BASE;
    } while (mantissa != 0);
    unsigned scale = power < 0 ? (unsigned)-power : 0;
    if (power < 0) {
        unsigned k = scale;
        while (k >= 12) {
            fp_multiply(limbs, &used, 244140625); /* 5^12 */
            k -= 12;
        }
        uint32_t tail = 1;
        while (k != 0) {
            tail *= 5;
            --k;
        }
        fp_multiply(limbs, &used, tail);
    } else {
        while (power >= 29) {
            fp_multiply(limbs, &used, UINT32_C(1) << 29);
            power -= 29;
        }
        fp_multiply(limbs, &used, UINT32_C(1) << power);
    }

    unsigned top = 1;
    for (uint32_t v = limbs[used - 1]; v >= 10; v /= 10)
        ++top;
    int length = (int)((used - 1) * 9 + top);
    char *end = out->digits + length;
    for (unsigned i = 0; i < used; ++i) {
        uint32_t limb = limbs[i];
        for (unsigned j = 0; j < (i == used - 1 ? top : 9); ++j) {
            *--end = (char)('0' + limb % 10);
            limb /= 10;
        }
    }
    out->length = length;
    out->point = length - (int)scale;
    fp_trim(out);
}

#undef CEPHYR_PRINTF_LIMB_BASE

/* Round to `keep` leading digits. With fixed precision the rounding place
   can precede the first nonzero digit (keep <= 0). */
static void fp_decimal_round(struct fp_output *out, int64_t keep)
{
    if (keep >= out->length || out->digits[0] == '0')
        return;
    char first = keep < 0 ? '0' : out->digits[keep];
    /* The significand was trimmed, so its last digit is nonzero. */
    int half = first < '5' ? -1 : first > '5' || keep + 1 < out->length;
    int odd = keep > 0 && (out->digits[keep - 1] - '0') % 2;
    int carry = fp_round_up(out->sign, half, odd);
    if (keep <= 0) {
        out->length = 1;
        out->digits[0] = carry ? '1' : '0';
        out->point = carry ? out->point - (int)keep + 1 : 1;
        return;
    }
    out->length = (int)keep;
    if (carry) {
        int i = out->length;
        while (i > 0 && out->digits[i - 1] == '9')
            out->digits[--i] = '0';
        if (i > 0)
            ++out->digits[i - 1];
        else {
            out->digits[0] = '1';
            ++out->point;
        }
    }
    fp_trim(out);
}

static void fp_decimal(struct fp_output *out, uint64_t mantissa, int power,
                       char conversion, int precision, int alternate)
{
    int scientific = 0;
    int64_t p = precision < 0 ? 6 : precision;
    fp_decimal_exact(out, mantissa, power);
    if (conversion == 'f' || conversion == 'F') {
        fp_decimal_round(out, out->point + p);
    } else if (conversion == 'e' || conversion == 'E') {
        fp_decimal_round(out, p + 1);
        scientific = 1;
    } else {
        if (p == 0)
            p = 1;
        fp_decimal_round(out, p);
        /* Select the style from the exponent *after* rounding. */
        scientific = out->point <= -4 || p < out->point;
        if (alternate)
            p -= scientific ? 1 : out->point;
        else {
            p = out->length - (scientific ? 1 : out->point);
            if (p < 0)
                p = 0;
        }
    }
    out->fraction = (size_t)p;
    if (scientific) {
        fp_exponent(out, out->point - 1, out->uppercase ? 'E' : 'e', 2);
        out->point = 1;
    }
}

static void fp_prepare(struct fp_output *out, double value, char conversion,
                       int precision, int alternate, int plus, int blank)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    int raw = (int)((bits >> 52) & 0x7ff);
    uint64_t mantissa = bits & ((UINT64_C(1) << 52) - 1);
    out->sign = bits >> 63 ? '-' : plus ? '+' : blank ? ' ' : '\0';
    out->uppercase = conversion == 'A' || conversion == 'E' ||
                     conversion == 'F' || conversion == 'G';
    if (raw == 0x7ff) {
        const char *name = mantissa ? (out->uppercase ? "NAN" : "nan")
                                    : (out->uppercase ? "INF" : "inf");
        memcpy(out->digits, name, 3);
        out->length = out->point = 3;
        out->special = 1;
        return;
    }
    if (raw != 0)
        mantissa |= UINT64_C(1) << 52;
    int exponent = (raw != 0 ? raw : 1) - 1023;
    if (conversion == 'a' || conversion == 'A')
        fp_hex(out, mantissa, mantissa != 0 ? exponent : 0, precision);
    else
        fp_decimal(out, mantissa, exponent - 52, conversion, precision, alternate);
}

#endif
