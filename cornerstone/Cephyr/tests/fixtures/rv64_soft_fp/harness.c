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

static U32 bits32(float x) { return ((F32){.value = x}).bits; }
static U64 bits64(double x) { return ((F64){.value = x}).bits; }

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
    return 0;
}
