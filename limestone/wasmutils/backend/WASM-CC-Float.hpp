#pragma once

#include <string_view>

namespace limestone::wasmutils::detail {

inline constexpr std::string_view module_float_support = R"C(
/* Arithmetic requires IEC 60559 binary32/binary64, evaluated at the
 * destination precision, with round-to-nearest (ties to even), gradual
 * underflow and disabled floating-point exceptions. Do not use fast-math.
 */
#include <float.h>
#include <limits.h>
#include <math.h>

static int wasm_fp_layout(const unsigned char *raw, const unsigned char *lsb,
                          unsigned n)
{
    unsigned i;
    if (memcmp(raw, lsb, n) == 0)
        return 1;
    for (i = 0; i < n; ++i)
        if (raw[n - 1U - i] != lsb[i])
            return 0;
    return -1;
}

static int wasm_float_order(void)
{
    static const float marker = 1.13679349422454833984375F;
    static const unsigned char bits[] = {0x73, 0x82, 0x91, 0x3f};
    if (sizeof(marker) != sizeof(bits))
        return 0;
    return wasm_fp_layout((const unsigned char *)&marker, bits, sizeof(bits));
}

static int wasm_double_order(void)
{
    static const double marker = 1.0629911572472126035648898323415778577327728271484375;
    static const unsigned char bits[] = {0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0xf1, 0x3f};
    if (sizeof(marker) != sizeof(bits))
        return 0;
    return wasm_fp_layout((const unsigned char *)&marker, bits, sizeof(bits));
}

static void wasm_fp_decode(unsigned char *raw, struct wasm_word a, unsigned n, int order)
{
    unsigned i;
    for (i = 0; i < n; ++i) {
        unsigned long limb = i < 4U ? a.lo : a.hi;
        raw[order > 0 ? i : n - 1U - i] = (unsigned char)(limb >> ((i % 4U) * 8U));
    }
}

static struct wasm_word wasm_fp_encode(const unsigned char *raw, unsigned n, int order)
{
    struct wasm_word a;
    unsigned i;
    a.lo = a.hi = 0UL;
    for (i = 0; i < n; ++i) {
        unsigned long *limb = i < 4U ? &a.lo : &a.hi;
        *limb |= (unsigned long)raw[order > 0 ? i : n - 1U - i] << ((i % 4U) * 8U);
    }
    return a;
}

static double wasm_fp_value(struct wasm_word a, int wide)
{
    if (wide) {
        double d;
        wasm_fp_decode((unsigned char *)&d, a, 8U, wasm_double_order());
        return d;
    } else {
        float f;
        wasm_fp_decode((unsigned char *)&f, a, 4U, wasm_float_order());
        return f;
    }
}

static struct wasm_word wasm_fp_bits(double a, int wide)
{
    if (wide) {
        return wasm_fp_encode((const unsigned char *)&a, 8U, wasm_double_order());
    } else {
        volatile float rounded = (float)a;
        float f = rounded;
        return wasm_fp_encode((const unsigned char *)&f, 4U, wasm_float_order());
    }
}

static int wasm_fp_nan(struct wasm_word a, int wide)
{
    return wide ? ((a.hi & 0x7ff00000UL) == 0x7ff00000UL &&
                   ((a.hi & 0xfffffUL) | a.lo) != 0UL) :
                  ((a.lo & 0x7fffffffUL) > 0x7f800000UL);
}

static struct wasm_word wasm_fp_canonical_nan(int wide)
{
    struct wasm_word result;
    result.lo = wide ? 0UL : 0x7fc00000UL;
    result.hi = wide ? 0x7ff80000UL : 0UL;
    return result;
}

static void wasm_fp_compare(struct wasm_word *a, struct wasm_word b, int wide, unsigned op)
{
    int result;
    if (wasm_fp_nan(*a, wide) || wasm_fp_nan(b, wide)) {
        result = op == 1U;
    } else {
        const double x = wasm_fp_value(*a, wide);
        const double y = wasm_fp_value(b, wide);
        switch (op) {
        case 0: result = x == y; break;
        case 1: result = x != y; break;
        case 2: result = x < y; break;
        case 3: result = x > y; break;
        case 4: result = x <= y; break;
        default: result = x >= y; break;
        }
    }
    a->lo = (unsigned long)result;
    a->hi = 0UL;
}

static struct wasm_word wasm_fp_arithmetic(struct wasm_word a, struct wasm_word b,
                                            int wide, unsigned op)
{
    const int binary = op >= 7U;
    const unsigned long sign = (wide ? a.hi : a.lo) & 0x80000000UL;
    double x, y, r;
    if (wasm_fp_nan(a, wide) || (binary && wasm_fp_nan(b, wide)))
        return wasm_fp_canonical_nan(wide);
    x = wasm_fp_value(a, wide);
    y = binary ? wasm_fp_value(b, wide) : 0.0;
    if (op == 11U || op == 12U) {
        if (x == y) {
            /* min(+0,-0) is -0; max(+0,-0) is +0. */
            if (wide)
                a.hi = op == 11U ? (a.hi | b.hi) : (a.hi & b.hi);
            else
                a.lo = op == 11U ? (a.lo | b.lo) : (a.lo & b.lo);
            return a;
        }
        return (op == 11U ? x < y : x > y) ? a : b;
    }
    if (binary) {
        if (wide) {
            volatile double rounded;
            switch (op) {
            case 7: rounded = x + y; break;
            case 8: rounded = x - y; break;
            case 9: rounded = x * y; break;
            default: rounded = x / y; break;
            }
            r = rounded;
        } else {
            volatile float fx = (float)x, fy = (float)y, rounded;
            switch (op) {
            case 7: rounded = fx + fy; break;
            case 8: rounded = fx - fy; break;
            case 9: rounded = fx * fy; break;
            default: rounded = fx / fy; break;
            }
            r = rounded;
        }
    } else {
        switch (op) {
        case 2: r = ceil(x); break;
        case 3: r = floor(x); break;
        case 4: r = sign ? ceil(x) : floor(x); break;
        case 5:
            if (x >= 4503599627370496.0 || x <= -4503599627370496.0)
                return a;
            r = floor(x);
            if (x - r > 0.5 || (x - r == 0.5 && fmod(r, 2.0) != 0.0))
                r += 1.0;
            break;
        default: r = sqrt(x); break;
        }
        if (r == 0.0) {
            a.lo = wide ? 0UL : sign;
            a.hi = wide ? sign : 0UL;
            return a;
        }
    }
    a = wasm_fp_bits(r, wide);
    return wasm_fp_nan(a, wide) ? wasm_fp_canonical_nan(wide) : a;
}

static struct wasm_word wasm_fp_resize(struct wasm_word a, int wide)
{
    if (wasm_fp_nan(a, !wide))
        return wasm_fp_canonical_nan(wide);
    return wasm_fp_bits(wasm_fp_value(a, !wide), wide);
}

static struct wasm_word wasm_fp_negate_integer(struct wasm_word a, int wide)
{
    a.lo = (0UL - a.lo) & 0xffffffffUL;
    a.hi = wide ? (0UL - a.hi - (a.lo != 0UL)) & 0xffffffffUL : 0UL;
    return a;
}

static int wasm_fp_to_integer(struct wasm_word *a, int src_wide, int dst_wide,
                              int sign, int saturating)
{
    const int negative = ((src_wide ? a->hi : a->lo) & 0x80000000UL) != 0UL;
    const double bound = dst_wide ? (sign ? 9223372036854775808.0 : 18446744073709551616.0)
                                 : (sign ? 2147483648.0 : 4294967296.0);
    double abs;
    struct wasm_word result;
    result.lo = result.hi = 0UL;
    if (wasm_fp_nan(*a, src_wide)) {
        if (!saturating)
            return WASM_TRAP_INVALID_CONVERSION;
    } else {
        abs = wasm_fp_value(*a, src_wide);
        abs = floor(negative ? -abs : abs);
        if (negative ? (sign ? abs > bound : abs != 0.0) : abs >= bound) {
            if (!saturating)
                return WASM_TRAP_INTEGER_OVERFLOW;
            if (!negative) {
                result.lo = !dst_wide && sign ? 0x7fffffffUL : 0xffffffffUL;
                if (dst_wide)
                    result.hi = sign ? 0x7fffffffUL : 0xffffffffUL;
            } else if (sign) {
                result.lo = dst_wide ? 0UL : 0x80000000UL;
                result.hi = dst_wide ? 0x80000000UL : 0UL;
            }
        } else {
            if (dst_wide) {
                result.hi = (unsigned long)(abs / 4294967296.0);
                abs -= (double)result.hi * 4294967296.0;
            }
            result.lo = (unsigned long)abs;
            if (negative)
                result = wasm_fp_negate_integer(result, dst_wide);
        }
    }
    *a = result;
    return WASM_OK;
}

/* Round the integer directly to the destination significand. In particular,
 * i64 -> f32 must not round via double first.
 */
static struct wasm_word wasm_integer_to_fp(struct wasm_word a, int src_wide, int dst_wide, int sign)
{
    int negative = sign && ((src_wide ? a.hi : a.lo) & 0x80000000UL) != 0UL;
    unsigned long precision = dst_wide ? 53UL : 24UL;
    unsigned long n = 0UL, count;
    int sticky = 0, half = 0;
    struct wasm_word v;
    if ((a.lo | a.hi) == 0UL)
        return a;
    if (negative)
        a = wasm_fp_negate_integer(a, src_wide);
    v = a;
    for (; (v.lo | v.hi) != 0UL; ++n)
        v = wasm_shr64(v, 1UL, 0UL);
    if (n > precision) {
        count = n - precision;
        while (count-- != 0UL) {
            sticky |= half;
            half = (a.lo & 1UL) != 0UL;
            a = wasm_shr64(a, 1UL, 0UL);
        }
        if (half && (sticky || (a.lo & 1UL))) {
            v.lo = 1UL;
            v.hi = 0UL;
            a = wasm_add64(a, v);
            if (dst_wide ? (a.hi & 0x200000UL) != 0UL : (a.lo & 0x1000000UL) != 0UL) {
                a = wasm_shr64(a, 1UL, 0UL);
                ++n;
            }
        }
    } else {
        a = wasm_shl64(a, precision - n);
    }
    if (dst_wide)
        a.hi = (a.hi & 0xfffffUL) | ((n + 1022UL) << 20);
    else
        a.lo = (a.lo & 0x7fffffUL) | ((n + 126UL) << 23);
    if (negative) {
        if (dst_wide)
            a.hi |= 0x80000000UL;
        else
            a.lo |= 0x80000000UL;
    }
    return a;
}

static int wasm_fp_supported(void)
{
    /* The emitter may use only a subset of the scalar FP operations. */
    (void)wasm_fp_compare;
    (void)wasm_fp_arithmetic;
    (void)wasm_fp_resize;
    (void)wasm_fp_to_integer;
    (void)wasm_integer_to_fp;
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
    return 0;
#else
    return CHAR_BIT == 8 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128 &&
           FLT_MIN_EXP == -125 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024 &&
           DBL_MIN_EXP == -1021 && wasm_float_order() && wasm_double_order();
#endif
}
)C";

} // namespace limestone::wasmutils::detail
