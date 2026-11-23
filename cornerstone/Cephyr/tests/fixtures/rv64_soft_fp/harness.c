typedef unsigned int U32;
typedef unsigned long U64;
typedef union { U32 bits; float value; } F32;
typedef union { U64 bits; double value; } F64;

extern volatile float vf[2];
extern volatile double vd[2];
extern float sf;
extern double sd;
extern float store_float(float);
extern double store_double(double);
extern float neg_float(float);
extern double neg_double(double);
extern float literal_float(void);
extern double literal_double(void);
extern int cmp_float(float, float);
extern int cmp_double(double, double);

typedef float F(long, float, double, long, long, long, long, long, float, double);
typedef double D(float, long, double, long, long, long, long, long, double, float);
extern F receive_float;
extern D receive_double;
extern float call_float(void);
extern double call_double(D *);

/* Do not let Clang turn bit-pattern checks into floating comparisons. */
static U32 bits32(float x) { volatile F32 v = {.value = x}; return v.bits; }
static U64 bits64(double x) { volatile F64 v = {.value = x}; return v.bits; }

#define ARITHMETIC(T, suffix)                      \
    extern T add_##suffix(T, T), sub_##suffix(T, T), mul_##suffix(T, T), div_##suffix(T, T); \
    extern T from_signed_##suffix(long), from_unsigned_##suffix(U64); \
    extern long to_signed_##suffix(T);             \
    extern U64 to_unsigned_##suffix(T);
ARITHMETIC(float, f)
ARITHMETIC(double, d)

extern double widen_float(float);
extern float narrow_double(double);
extern float from_unsigned_int(unsigned int);
extern signed char to_signed_char(double);
extern unsigned int to_unsigned_int(float);
extern double mixed_math(float, unsigned int, double);
extern float compound_math(float *, float);

/* This TU also uses Clang's own RV64IM helper lowering. */
float clang_add(float x, float y) { return x + y; }

float clang_float(long pick, float a, double b, long x, long y, long z,
                  long u, long v, float stack_float, double stack_double) {
    if (bits32(a) != 0xbf800000U || bits64(b) != 0xc000000000000000UL ||
        x != 4 || y != 5 || z != 6 || u != 7 || v != 8 ||
        bits64(stack_double) != 0xbff0000000000000UL)
        return 0.0f;
    return pick ? a : stack_float;
}

double clang_double(float a, long pick, double b, long x, long y, long z,
                    long u, long v, double stack_double, float stack_float) {
    if (bits32(a) != 0xbf800000U || bits64(b) != 0xc000000000000000UL ||
        x != 4 || y != 5 || z != 6 || u != 7 || v != 8 ||
        bits32(stack_float) != 0xbf800000U)
        return 0.0;
    return pick ? b : stack_double;
}

/* Sorted by value up to the NaN tail. Indices 4/5 are equal signed zeros.
 * Compare the whole cross product without needing external FP helpers. */
static const F32 values32[] = {{0xff800000}, {0xc0000000}, {0xbf800000},
    {0x80000001}, {0x80000000}, {0}, {1}, {0x3f800000}, {0x7f800000},
    {0x7fc12345}, {0xff800001}};
static const F64 values64[] = {{0xfff0000000000000UL}, {0xc000000000000000UL},
    {0xbff0000000000000UL}, {0x8000000000000001UL}, {0x8000000000000000UL},
    {0}, {1}, {0x3ff0000000000000UL}, {0x7ff0000000000000UL},
    {0x7ff8000000012345UL}, {0xfff0000000000001UL}};

static int expected(int i, int j) {
    int eq = i == j || ((i == 4 || i == 5) && (j == 4 || j == 5));
    int truth = i != 4 && i != 5;
    if (i >= 9 || j >= 9)
        return 8 | (truth << 6);
    return eq | ((!eq && i < j) << 1) | ((eq || i < j) << 2) |
           (!eq << 3) | ((!eq && i > j) << 4) | ((eq || i > j) << 5) | (truth << 6);
}

int test_main(void) {
    if (bits32(vf[0]) != 0x80000000 || bits64(vd[0]) != 0x8000000000000000UL ||
        bits32(sf) != 0x3f400000 || bits64(sd) != 0x3f58000000000000UL)
        return 1;
    if (bits32(literal_float()) != 0xbfa00000 || bits64(literal_double()) != 0xc004000000000000UL)
        return 2;
    for (int i = 0; i < 11; ++i) {
        if (bits32(store_float(values32[i].value)) != values32[i].bits ||
            bits64(store_double(values64[i].value)) != values64[i].bits ||
            bits32(vf[1]) != 1 || bits64(vd[1]) != 1)
            return 3;
        if (bits32(neg_float(values32[i].value)) != (values32[i].bits ^ 0x80000000) ||
            bits64(neg_double(values64[i].value)) != (values64[i].bits ^ 0x8000000000000000UL))
            return 4;
        for (int j = 0; j < 11; ++j) {
            if (cmp_float(values32[i].value, values32[j].value) != expected(i, j))
                return 5;
            if (cmp_double(values64[i].value, values64[j].value) != expected(i, j))
                return 6;
        }
    }
    for (long pick = 0; pick <= 1; ++pick) {
        if (bits32(receive_float(pick, -1.0f, -2.0, 4, 5, 6, 7, 8, -3.25f, -1.0)) !=
            (pick ? 0xbf800000U : 0xc0500000U))
            return 7;
        if (bits64(receive_double(-1.0f, pick, -2.0, 4, 5, 6, 7, 8, -3.25, -1.0f)) !=
            (pick ? 0xc000000000000000UL : 0xc00a000000000000UL))
            return 8;
    }
    if (bits32(call_float()) != 0xc0500000 || bits64(call_double(clang_double)) != 0xc00a000000000000UL)
        return 9;
    /* Expected bits are independent of the software arithmetic implementation. */
    if (bits32(add_f(1.0f, 0x1p-24f)) != 0x3f800000 ||
        bits32(add_f(0x1.000002p0f, 0x1p-24f)) != 0x3f800002 ||
        bits32(sub_f(0x1p-126f, 0x1.fffffcp-127f)) != 1 ||
        bits32(mul_f(0x1p-126f, -0x1p-24f)) != 0x80000000 ||
        bits32(mul_f(0x1.8p-126f, 0x1p-23f)) != 2 ||
        bits32(div_f(-1.0f, 3.0f)) != 0xbeaaaaab ||
        bits32(div_f(0x1p-149f, 1.0f)) != 1)
        return 10;
    if (bits64(add_d(1.0, 0x1p-53)) != 0x3ff0000000000000UL ||
        bits64(add_d(0x1.0000000000001p0, 0x1p-53)) != 0x3ff0000000000002UL ||
        bits64(sub_d(0x1p-1022, 0x1.ffffffffffffep-1023)) != 1 ||
        bits64(mul_d(0x1p-1022, -0x1p-53)) != 0x8000000000000000UL ||
        bits64(mul_d(0x1.8p-1022, 0x1p-52)) != 2 ||
        bits64(div_d(-1.0, 3.0)) != 0xbfd5555555555555UL ||
        bits64(div_d(0x1p-1074, 1.0)) != 1)
        return 11;
    if (bits32(add_f(0x1.fffffep127f, 0x1.fffffep127f)) != 0x7f800000 ||
        bits64(div_d(1.0, -0.0)) != 0xfff0000000000000UL ||
        (bits32(mul_f(values32[8].value, 0.0f)) & 0x7fffffff) <= 0x7f800000 ||
        (bits64(add_d(values64[9].value, 1.0)) & 0x7fffffffffffffffUL) <= 0x7ff0000000000000UL)
        return 12;
    if (bits64(widen_float(-0x1p-149f)) != 0xb6a0000000000000UL ||
        bits64(widen_float(-0.0f)) != 0x8000000000000000UL ||
        bits32(narrow_double(0x1.000001p0)) != 0x3f800000 ||
        bits32(narrow_double(0x1.000003p0)) != 0x3f800002 ||
        bits32(narrow_double(0x1.8p-149)) != 2 ||
        bits32(narrow_double(0x1.ffffffp127)) != 0x7f800000)
        return 13;
    if (bits32(from_signed_f(-16777219L)) != 0xcb800002 ||
        bits32(from_signed_f(-0x7fffffffffffffffL - 1)) != 0xdf000000 ||
        bits32(from_unsigned_f(~0UL)) != 0x5f800000 ||
        bits64(from_signed_d(-0x7fffffffffffffffL - 1)) != 0xc3e0000000000000UL ||
        bits64(from_unsigned_d(~0UL)) != 0x43f0000000000000UL ||
        bits32(from_unsigned_int(0x80000000U)) != 0x4f000000)
        return 14;
    if (to_signed_f(-3.75f) != -3 || to_signed_d(-3.75) != -3 ||
        to_signed_f(-0x1p63f) != -0x7fffffffffffffffL - 1 ||
        to_signed_d(-0x1p63) != -0x7fffffffffffffffL - 1 ||
        to_unsigned_f(0x1p63f) != 0x8000000000000000UL ||
        to_unsigned_d(0x1.fffffffffffffp63) != 0xfffffffffffff800UL ||
        to_unsigned_d(-0.75) != 0 ||
        to_signed_char(-127.75) != -127 || to_unsigned_int(0x1p31f) != 0x80000000U)
        return 15;
    float local = 1.5f;
    if (bits64(mixed_math(1.0f, 7, 2.0)) != 0x4030400000000000UL ||
        bits32(compound_math(&local, 3.0f)) != 0x40c00000 ||
        bits32(local) != 0x40f00000)
        return 16;
    return 0;
}

int main(void) { return test_main(); }
