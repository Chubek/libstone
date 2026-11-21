#include <float.h>

extern double not_called(void);

static float binary32[] = {
    (0x1p24f + 1.f) - 0x1p24f,             /* round each operator, not just assignment */
    (0x1p24f + 1.) - 0x1p24f,              /* mixed arithmetic promotes to double */
    0x1p-149f / 2.f,                       /* tie rounds to even zero */
    0x1.8p-149f,                           /* literal rounds to even subnormal */
    0x1p-126f - 0x1p-149f,                 /* largest subnormal */
    0x1.fffffcp-127f + 0x1p-149f,          /* carry to smallest normal */
    -(float)0,                             /* unary minus after an integer-to-FP cast */
    0.f - 0.f,                            /* different from unary minus */
    -0.f + -0.f,
    -0.f * -2.f,
    -0x1p-149f / 2.f,                      /* underflow retains sign */
    (float)((0x1p24f + 1.f) + 0x1p24),
    (float)(2.0 + (long)3.125),
    1.f / 3.f,
    (float)18446744073709551615ULL,
    FLT_MAX - FLT_MAX,
};

static struct Floats { double div, mul, conditional, generic, size, narrowed, subnormal; } binary64 = {
    .div = 1.0 / 10.0,
    .mul = 0x1.0000002p0 * 0x1.ffffffcp-1, /* (1+2^-27)(1-2^-27) rounds to one */
    .conditional = -0.0 ? (1. / 0.) : +2.f * 3.5,
    .generic = _Generic((1.f + 1.f), float: 3. / 2., default: 0.),
    .size = (double)sizeof(0.0 / 0.0) + _Alignof(float),
    .narrowed = 1 ? 16777217U : -1.f,      /* conditional conversion precedes assignment */
    .subnormal = DBL_MIN - DBL_TRUE_MIN,
};

enum { ice = (int)2.75 };
static unsigned ints[] = {
    (int)(4.75 + 1.0),
    2.8,
    -0.5,                                  /* valid truncated unsigned conversion to zero */
    -0.0 == 0.0,
    2.f != 2,
    -3. < -2.f,
    -3. <= -3.f,
    2 > -2.f,
    2. >= 3,
    0.0 || -1.f,
    -0.f && (int)(1.0 / 0.0),
    !-0.0,
    3. ? (short)4 : (int)(1.0 / 0.0),
    4294967295U < 4294967296.,
    16777217U == 16777216.f,                /* integer rounds in usual arithmetic conversions */
    1 ? 7 : not_called(),                 /* unselected call needn't link or be evaluated */
    1 ? 8 : (0, 3),
};
static _Bool b = 0.25, z = -0.0;

static unsigned long read_bits(const void *raw, unsigned width) {
    const unsigned char *p = raw;
    unsigned long r = 0;
    for (unsigned i = 0; i < width; ++i)
        r |= (unsigned long)p[i] << (i * 8);
    return r;
}

int test_main(void) {
    enum { local_enum = ice + 5 };
    static double local = local_enum + 0.25 * 2.;
    static const unsigned expected32[] = {
        0, 0x3f800000U, 0, 2, 0x007fffffU, 0x00800000U,
        0x80000000U, 0, 0x80000000U, 0, 0x80000000U, 0x4c000000U,
        0x40a00000U, 0x3eaaaaabU, 0x5f800000U, 0,
    };
    static const unsigned expected_ints[] = {5, 2, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 4, 1, 1, 7, 8};
    for (unsigned i = 0; i < sizeof(expected32) / sizeof(*expected32); ++i)
        if (read_bits(&binary32[i], 4) != expected32[i])
            return i + 1;
    if (read_bits(&binary64.div, 8) != 0x3fb999999999999aUL) return 31;
    if (read_bits(&binary64.mul, 8) != 0x3ff0000000000000UL) return 32;
    if (binary64.conditional != 7.) return 33;
    if (binary64.generic != 1.5) return 34;
    if (binary64.size != 12.) return 35;
    if (local != 7.5 || !b || z) return 36;
    if (binary64.narrowed != 16777216.0) return 37;
    if (read_bits(&binary64.subnormal, 8) != 0x000fffffffffffffUL) return 38;
    for (unsigned i = 0; i < sizeof(expected_ints) / sizeof(*expected_ints); ++i)
        if (ints[i] != expected_ints[i])
            return i + 40;
    return 0;
}
