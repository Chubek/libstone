typedef float (*Ternary)(float, int, double);

#ifndef __CEPHYR_HAS_SCALAR_FP
#error "reviewed target did not advertise native scalar FP"
#endif

static float rounded = 0x1.00000100000000000001p0f;
static float from_double = 1.000000059604644775390625000000000001;
static double from_integer = 9007199254740993UL;
static double neg_zero = -0.0;
static const float min_subnormal = 0x1p-149f;
static const double underflow_zero = -1e-9999999;
static float tentative;

_Static_assert(sizeof(float) == 4 && _Alignof(float) == 4, "float layout");
_Static_assert(sizeof(double) == 8 && _Alignof(double) == 8, "double layout");
_Static_assert((int)0x1.8p1 == 3, "literal integer constant expression");
_Static_assert(_Generic(0.25f + 1, float: 1, default: 0), "float mixed type");
_Static_assert(_Generic(0.25f + 1.0, double: 1, default: 0), "double mixed type");

float ceph_negate(float x) { return -x; }
double ceph_negate_double(double x) { return -x; }
float ceph_neg_zero_subtract(float x) { return 0.0f - x; }
float ceph_globals(void) { return rounded - from_double + tentative; }
double ceph_globals_double(void) { return from_integer; }
double ceph_static_neg_zero(void) { return neg_zero; }
double ceph_underflow_zero(void) { return underflow_zero; }
float ceph_static_subnormal(void) { return min_subnormal; }

float ceph_formula(float a, int b, double c)
{
    float x = +a;
    float *p = &x;
    *p += b;
    return (float)((*p * 2.0 - c) / 3.0);
}

double ceph_choose(int condition, float f, double d) { return condition ? f : d; }
unsigned long ceph_unsigned(double d) { return (unsigned long)d; }
long ceph_signed(float f) { return (long)f; }
double ceph_from_unsigned(unsigned long u) { return u; }
int ceph_truth(float f) { return (_Bool)f && !!f; }
int ceph_implicit_integer(void) { return 1.5; }

struct Storage { float first; double second; };
static struct Storage pair = { .first = -2.5f, .second = 1.5 };

float ceph_record(void)
{
    struct Storage copy = pair;
    float old = copy.first++;
    return old + copy.first + (float)copy.second;
}

unsigned ceph_compare(double f, float g)
{
    return (f == g) + ((f != g) << 1) + ((f < g) << 2) +
           ((f <= g) << 3) + ((f > g) << 4) + ((f >= g) << 5);
}

extern float host_formula(float, int, double);

float ceph_call(Ternary callback, float x)
{
    return ceph_formula(x, 2, 3.0) + callback(x, -2, 15.0) +
           host_formula(x, -3, 9.0);
}
