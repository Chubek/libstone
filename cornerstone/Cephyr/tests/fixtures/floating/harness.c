typedef float (*Ternary)(float, int, double);

extern float ceph_negate(float);
extern double ceph_negate_double(double);
extern float ceph_neg_zero_subtract(float);
extern float ceph_globals(void);
extern double ceph_globals_double(void);
extern double ceph_static_neg_zero(void);
extern double ceph_underflow_zero(void);
extern float ceph_static_subnormal(void);
extern float ceph_formula(float, int, double);
extern double ceph_choose(int, float, double);
extern unsigned long ceph_unsigned(double);
extern long ceph_signed(float);
extern double ceph_from_unsigned(unsigned long);
extern int ceph_truth(float);
extern int ceph_implicit_integer(void);
extern float ceph_record(void);
extern unsigned ceph_compare(double, float);
extern float ceph_call(Ternary, float);

static unsigned bits32(float f)
{
    unsigned bits;
    __builtin_memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static unsigned long bits64(double f)
{
    unsigned long bits;
    __builtin_memcpy(&bits, &f, sizeof(bits));
    return bits;
}

float host_formula(float a, int b, double c) { return (float)((a + b) * 0.5 + c); }

int test_main(void)
{
    if (bits32(ceph_negate(0.0f)) != 0x80000000U ||
        bits64(ceph_negate_double(0.0)) != 0x8000000000000000UL ||
        bits32(ceph_neg_zero_subtract(0.0f)) != 0 ||
        bits64(ceph_static_neg_zero()) != 0x8000000000000000UL ||
        bits64(ceph_underflow_zero()) != 0x8000000000000000UL)
        return 1;
    if (ceph_globals() != 0x1p-23f || ceph_globals_double() != 0x1p53 ||
        bits32(ceph_static_subnormal()) != 1)
        return 2;
    if (ceph_formula(1.75f, -2, 6.25) != -2.25f || ceph_choose(0, 1.5f, 5.25) != 5.25 ||
        ceph_choose(1, 1.5f, 5.25) != 1.5)
        return 3;
    if (ceph_unsigned(0x1.fffffffffffffp63) != 0xfffffffffffff800UL ||
        ceph_signed(-37.75f) != -37 || ceph_from_unsigned(~0UL) != 0x1p64)
        return 4;
    if (ceph_truth(-0.0f) || !ceph_truth(-0.125f) || !ceph_truth(__builtin_nanf("1")))
        return 5;
    if (ceph_compare(3.0, 3.0f) != 41 || ceph_compare(2.0, 3.0f) != 14 ||
        ceph_compare(5.0, 3.0f) != 50 || ceph_compare(__builtin_nan("1"), 3.0f) != 2)
        return 6;
    if (ceph_call(host_formula, 2.5f) !=
        ((float)(((2.5f + 2) * 2.0 - 3.0) / 3.0) +
            host_formula(2.5f, -2, 15.0) + host_formula(2.5f, -3, 9.0)))
        return 7;
    if (ceph_implicit_integer() != 1 || ceph_record() != -2.5f)
        return 8;
    return 0;
}
