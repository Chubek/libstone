#ifndef __riscv_float_abi_soft
#error expected RISC-V soft-float
#endif
#ifdef __CEPHYR_HAS_SCALAR_FP
#error partial RV64 soft-float must not advertise full scalar FP
#endif

_Static_assert(sizeof(float) == 4 && _Alignof(float) == 4, "binary32 layout");
_Static_assert(sizeof(double) == 8 && _Alignof(double) == 8, "binary64 layout");

volatile float vf[2] = {-0.0f, 0x1p-149f};
volatile double vd[2] = {-0.0, 0x1p-1074};
float sf = 0.5f + 0.25f;
double sd = 0x1p-10 + 0x1p-11;

float store_float(float x) {
    vf[0] = x;
    return vf[0];
}

double store_double(double x) {
    vd[0] = x;
    return vd[0];
}

float neg_float(float x) { return -x; }
double neg_double(double x) { return -x; }
float literal_float(void) { return -1.25f; }
double literal_double(void) { return -2.5; }

int cmp_float(float a, float b) {
    return (a == b) | ((a < b) << 1) | ((a <= b) << 2) |
           ((a != b) << 3) | ((a > b) << 4) | ((a >= b) << 5) | (!!a << 6);
}

int cmp_double(double a, double b) {
    return (a == b) | ((a < b) << 1) | ((a <= b) << 2) |
           ((a != b) << 3) | ((a > b) << 4) | ((a >= b) << 5) | (!!a << 6);
}

typedef float F(long, float, double, long, long, long, long, long, float, double);
typedef double D(float, long, double, long, long, long, long, long, double, float);
extern F clang_float;
extern D clang_double;

float receive_float(long pick, float a, double b, long x, long y, long z,
                    long u, long v, float stack_float, double stack_double) {
    if (b <= stack_double && x == 4 && y == 5 && z == 6 && u == 7 && v == 8)
        return pick ? a : stack_float;
    return 0.0f;
}

double receive_double(float a, long pick, double b, long x, long y, long z,
                      long u, long v, double stack_double, float stack_float) {
    if (a <= stack_float && x == 4 && y == 5 && z == 6 && u == 7 && v == 8)
        return pick ? b : stack_double;
    return 0.0;
}

float call_float(void) {
    return clang_float(0, -1.0f, -2.0, 4, 5, 6, 7, 8, -3.25f, -1.0);
}

double call_double(D *callback) {
    return callback(-1.0f, 0, -2.0, 4, 5, 6, 7, 8, -3.25, -1.0f);
}

#define ARITHMETIC(T, suffix)                      \
    T add_##suffix(T x, T y) { return x + y; }      \
    T sub_##suffix(T x, T y) { return x - y; }      \
    T mul_##suffix(T x, T y) { return x * y; }      \
    T div_##suffix(T x, T y) { return x / y; }      \
    T from_signed_##suffix(long x) { return x; }   \
    T from_unsigned_##suffix(unsigned long x) { return x; } \
    long to_signed_##suffix(T x) { return x; }     \
    unsigned long to_unsigned_##suffix(T x) { return x; }
ARITHMETIC(float, f)
ARITHMETIC(double, d)

double widen_float(float x) { return x; }
float narrow_double(double x) { return x; }
float from_unsigned_int(unsigned int x) { return x; }
signed char to_signed_char(double x) { return x; }
unsigned int to_unsigned_int(float x) { return x; }

extern float clang_add(float, float);

double mixed_math(float x, unsigned int y, double z) {
    double r = clang_add(x, 0.5f);  /* Clang's helper call, then Cephyr's. */
    return (r + y) * z - r / 2;
}

float compound_math(float *p, float x) {
    float old = (*p)++;
    *p *= x;
    return *p - old;
}
