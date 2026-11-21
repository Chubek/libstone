"""Verify Cephyr's SysV AMD64 x87 objects and operations against external C."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


HEADERS = Path(__file__).resolve().parents[1] / "stdlib" / "include"
DECLARATIONS = r"""
#include <float.h>
#include <stddef.h>
struct Packet {
    unsigned char tag;
    long double value;
    unsigned short end;
};
union Slot { char c; long double value; };
_Static_assert(LDBL_MANT_DIG == 64 && LDBL_MAX_EXP == 16384, "x87 float.h");
_Static_assert(FLT_RADIX == 2 && LDBL_HAS_SUBNORM == 1, "x87 format");
_Static_assert(sizeof(long double) == 16 && _Alignof(long double) == 16, "scalar");
_Static_assert(sizeof(union Slot) == 16 && _Alignof(union Slot) == 16, "union");
_Static_assert(sizeof(struct Packet) == 48 && _Alignof(struct Packet) == 16, "record");
_Static_assert(offsetof(struct Packet, value) == 16, "record member");
_Static_assert(offsetof(struct Packet, end) == 32, "member after padding");
_Static_assert(sizeof(long double[3]) == 48, "array stride");
_Static_assert(_Generic(1.L, double: 0, long double: 1), "literal type");
_Static_assert(_Generic(1.L + 1.f, float: 0, double: 0, long double: 1), "usual conversion");
_Static_assert((int)3.875L == 3, "literal integer cast");
_Static_assert((unsigned long long)0x1.fffffffffffffffep63L == 18446744073709551615ULL,
               "literal integer cast at unsigned limit");
"""
VALUES = r"""
    0x1.0000000000000002p0L,
    1.000000000000000000108420217248550443400745280086994171142578125L,
    0x1.0000000000000001p0L,
    0x1.00000000000000010001p0L,
    LDBL_MAX, -0.L, LDBL_MIN, -LDBL_TRUE_MIN,
    0x0.ffffffffffffffffp-16382L,
    0x1.8p-16446L,
    18446744073709551615ULL,
    0x1p-1074, 0x1p-149F,
    (1.L + LDBL_EPSILON) - 1.L,
    (0x1.0000000000000002p0L * 2.L) - 2.L,
    1.L / 3.L,
    LDBL_MIN * 0x1p-63L,
    0x1.fp-8223L * 0x1.fp-8224L,
    -0.L ? 0.L : 1.L,
    0x1.0000000000000002p0L > 1.0 ? -2.L : 0.L,
    1e-4940L
"""
RUNTIME = r"""
void transfer(volatile long double *out, const volatile long double *in, unsigned *reads) {
    unsigned index = 0;
    const long double temp = in[index++];
    struct Packet box = { 11, +temp, 22 };
    out[0] = box.value;
    out[1] = out[2] = -in[index++];
    out[3] = -0x0p0L;
    out[4] = (long double)in[index++];
    out[5] = out[5];
    (void)in[index++];
    out[6] = 0x1.0000000000000002p0L;
    *reads = index;
}

void discard(const volatile long double *p) {
    (void)*p;
}

void calculate(volatile long double *out, const volatile long double *in) {
    long double x = in[0], y = in[1];
    out[0] = x + y;
    out[1] = x - y;
    out[2] = x * y;
    out[3] = x / y;
    unsigned i = 4;
    out[i++] += y;
    out[5] = x++;
    out[6] = --x;
    out[7] = x < y ? y : x;
    out[8] = x ? y + 0.5f : 7;
}

unsigned predicates(const volatile long double *in) {
    long double x = in[0], y = in[1];
    _Bool b = x;
    unsigned mask = (x == y) | ((x != y) << 1) |
        ((x < y) << 2) | ((x <= y) << 3) |
        ((x > y) << 4) | ((x >= y) << 5) |
        ((!x) << 6) | (b << 7) |
        ((x && y) << 8) | ((x || y) << 9);
    if (x)
        mask |= 1U << 10;
    return mask | ((x < 1.0) << 11) | ((y == 0) << 12);
}

void widen(long double *out, float a, double b, long long c, unsigned long long d) {
    long double x = a;
    out[0] = x;
    out[1] = b;
    out[2] = c;
    out[3] = d;
    out[4] = d + x;
    out[5] = (_Bool)c;
}

void narrow_values(float *f, double *d, long long *i, unsigned long long *u,
                   const volatile long double *in) {
    *f = (float)in[0];
    *d = in[1];
    *i = (long long)in[2];
    *u = in[3];
}

int narrow_ints(unsigned *u, const volatile long double *in) {
    *u = (unsigned)in[0];
    return (signed char)in[1] + (short)in[2];
}

void conditional_effects(long double *out, const volatile long double *in, unsigned *used) {
    unsigned i = 0;
    const long double x = in[i++];
    long double y = x ? in[i++] : -in[i++];
    out[0] = y;
    out[1] = x && in[i++];
    out[2] = x || in[i++];
    *used = i;
}
"""
NATIVE = DECLARATIONS + r"""
const long double table[] = { VALUES };
const struct Packet packet = { 7, 0x1.0000000000000002p0L, 0xabcd };
const union Slot slot = { .value = -0.L };
long double zeroed[2];
unsigned long long wide = (unsigned long long)(0x1.fffffffffffffffep63L - 1.L);
double narrow = 0x1.0000000000000802p0L;
float single = 0x1.0000010000000002p0L;
int consume(const volatile long double *, const struct Packet *);
int probe(void) {
    static const long double local_static = -LDBL_MAX;
    _Alignas(16) volatile long double local = 1.L / 3.L;
    struct Packet local_packet = { 7, 0x1.0000000000000002p0L, 0xabcd };
    volatile struct Packet intermediate = local_packet;
    local_packet = intermediate;
    const long double *p = table;
    if ((p + 2) - p != 2 || (unsigned long)&local_static % 16)
        return 1;
    return consume(&local, &local_packet) + (int)3.5L - 3;
}
""".replace("VALUES", VALUES) + RUNTIME
EXTERNAL = DECLARATIONS + r"""
#include <stdint.h>
#include <string.h>
#include <fenv.h>
#include <math.h>
extern const long double table[];
extern const struct Packet packet;
extern const union Slot slot;
extern long double zeroed[];
extern unsigned long long wide;
extern double narrow;
extern float single;
extern int probe(void);
void transfer(volatile long double *, const volatile long double *, unsigned *);
void reference_transfer(volatile long double *, const volatile long double *, unsigned *);
typedef void Calc(volatile long double *, const volatile long double *);
Calc calculate, reference_calculate;
typedef unsigned Pred(const volatile long double *);
Pred predicates, reference_predicates;
typedef void Widen(long double *, float, double, long long, unsigned long long);
Widen widen, reference_widen;
typedef void Narrow(float *, double *, long long *, unsigned long long *, const volatile long double *);
Narrow narrow_values, reference_narrow_values;
typedef int NarrowInts(unsigned *, const volatile long double *);
NarrowInts narrow_ints, reference_narrow_ints;
typedef void Effects(long double *, const volatile long double *, unsigned *);
Effects conditional_effects, reference_conditional_effects;

int consume(const volatile long double *p, const struct Packet *s) {
    if ((uintptr_t)p % 16)
        return 105;
    if (*p != 1.L / 3.L)
        return 106;
    if (s->tag != 7 || s->value != 0x1.0000000000000002p0L || s->end != 0xabcd)
        return 107;
    return 0;
}

static int same_values(const long double *a, const long double *b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (isnan(a[i]) && isnan(b[i]))
            continue; /* Arithmetic does not specify a NaN payload. */
        if (memcmp(&a[i], &b[i], 10))
            return 0;
    }
    return 1;
}

static int check_operations(void) {
    const int modes[] = { FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO };
    const long double pairs[][2] = {
        { 1.L, 0x1p-64L }, /* Halfway at x87 precision. */
        { 0x1.0000000000000002p0L, 1.L }, /* Distinct in x87, not in binary64. */
        { 5.L, -3.L },
        { LDBL_MIN, 0.125L },
        { LDBL_TRUE_MIN, 2.L },
        { -0.L, 3.L },
        { __builtin_nanl("0x42"), 2.L }
    };
    for (size_t m = 0; m < sizeof modes / sizeof *modes; ++m) {
        if (fesetround(modes[m]))
            return 130;
        for (size_t p = 0; p < sizeof pairs / sizeof *pairs; ++p) {
            long double actual[9], expect[9];
            memset(actual, 0xa5, sizeof actual);
            actual[4] = expect[4] = -3.L;
            calculate(actual, pairs[p]);
            reference_calculate(expect, pairs[p]);
            if (!same_values(actual, expect, 9))
                return 131;
            for (size_t j = 0; j < 9; ++j)
                for (size_t k = 10; k < sizeof actual[j]; ++k)
                    if (((unsigned char *)&actual[j])[k] != 0xa5)
                        return 132;
        }

        long double actual[6], expect[6];
        widen(actual, -0.0f, 0x1p-1074, (-9223372036854775807LL - 1), 18446744073709551615ULL);
        reference_widen(expect, -0.0f, 0x1p-1074, (-9223372036854775807LL - 1), 18446744073709551615ULL);
        if (!same_values(actual, expect, 6))
            return 133;
        widen(actual, 0x1p-149f, -0.0, -42, 9223372036854775809ULL);
        reference_widen(expect, 0x1p-149f, -0.0, -42, 9223372036854775809ULL);
        if (!same_values(actual, expect, 6))
            return 134;

        const long double casts[][4] = {
            /* Extra x87 bit above halfway catches a conversion via binary64. */
            {0x1.0000010000000002p0L, 0x1.0000000000000802p0L, -3.875L, 0x1.fffffffffffffffep63L},
            {-0x1.000001p0L, -0x1.00000000000008p0L, 3.875L, -0.875L},
            {0x1p-150L, -0x1p-1075L, -0x1p63L, 0x1p63L},
            {-0.L, -0.L, 0x1.fffffffffffffffcp62L, 0x1p63L - 0.5L}
        };
        for (size_t c = 0; c < sizeof casts / sizeof *casts; ++c) {
            float f, ef;
            double d, ed;
            long long i, ei;
            unsigned long long u, eu;
            narrow_values(&f, &d, &i, &u, casts[c]);
            if (fegetround() != modes[m])
                return 135;
            reference_narrow_values(&ef, &ed, &ei, &eu, casts[c]);
            if (memcmp(&f, &ef, sizeof f) || memcmp(&d, &ed, sizeof d) || i != ei || u != eu)
                return 136;
        }
        const long double ints[] = {4294967295.875L, -127.875L, 32767.875L};
        unsigned u, eu;
        if (narrow_ints(&u, ints) != reference_narrow_ints(&eu, ints) || u != eu)
            return 137;
    }

    if (fesetround(FE_TONEAREST))
        return 138;
    const long double vals[] = { -__builtin_infl(), -1.L, -LDBL_TRUE_MIN, -0.L,
        0.L, LDBL_TRUE_MIN, 1.L, 0x1.0000000000000002p0L, __builtin_infl(), __builtin_nanl("") };
    for (size_t i = 0; i < sizeof vals / sizeof *vals; ++i) {
        for (size_t j = 0; j < sizeof vals / sizeof *vals; ++j) {
            const long double pair[] = {vals[i], vals[j]};
            if (predicates(pair) != reference_predicates(pair))
                return 139;
        }
        const long double in[] = {vals[i], 2.L, -3.L};
        long double actual[3], expect[3];
        unsigned used, expected_used;
        conditional_effects(actual, in, &used);
        reference_conditional_effects(expect, in, &expected_used);
        if (used != 3 || used != expected_used || !same_values(actual, expect, 3))
            return 140;
    }
    return 0;
}

int main(void) {
    static const long double expect[] = { VALUES };
    for (size_t i = 0; i < sizeof expect / sizeof *expect; ++i)
        if (memcmp(&table[i], &expect[i], 10) != 0)
            return (int)i + 1;
    if ((uintptr_t)table % 16 || (uintptr_t)&packet % 16 || (uintptr_t)&slot % 16 ||
        (uintptr_t)zeroed % 16)
        return 101;
    if (packet.tag != 7 || packet.value != expect[0] || packet.end != 0xabcd ||
        memcmp(&slot.value, &expect[5], 10) || zeroed[1] != 0.L)
        return 102;
    if (wide != 18446744073709551614ULL ||
        narrow != (double)0x1.0000000000000802p0L || single != (float)0x1.0000010000000002p0L)
        return 103;
    const long double input[4] = { LDBL_MAX, LDBL_TRUE_MIN, __builtin_nanl("0x5a"), 1.L };
    long double actual[7], reference[7];
    memset(actual, 0xa5, sizeof actual);
    actual[5] = reference[5] = 0x1.0000000000000002p0L;
    unsigned reads = 0, reference_reads = 0;
    transfer(actual, input, &reads);
    reference_transfer(reference, input, &reference_reads);
    if (reads != 4 || reads != reference_reads)
        return 104;
    for (size_t i = 0; i < 7; ++i) {
        if (memcmp(&actual[i], &reference[i], 10))
            return 110 + i;
        /* Cephyr's scalar stores only write the 10 value bytes. */
        if (i != 5) {
            const unsigned char *bytes = (const unsigned char *)&actual[i];
            for (size_t j = 10; j < sizeof actual[i]; ++j)
                if (bytes[j] != 0xa5)
                    return 120 + i;
        }
    }
    const int status = probe();
    return status ? status : check_operations();
}
""".replace("VALUES", VALUES)

NEGATIVE = {
    "return": "long double f(void) { return 1.L; }",
    "parameter": "int f(long double x) { return 1; }",
    "call": "extern int g(long double); int f(void) { return g(1.L); }",
    "indirect": "int f(int (*g)(long double)) { return g(1.L); }",
    "pointer_to_real": "void f(long double *x, void *p) { *x = (long double)p; }",
    "real_to_pointer": "void *f(const long double *x) { return (void *)*x; }",
    "remainder": "void f(long double *x) { *x %= 2; }",
    "const_assignment": "void f(const long double *x) { *x = 0.L; }",
    "variadic": "extern int g(int, ...); int f(void) { return g(0, 1.L); }",
    "va_arg": "#include <stdarg.h>\nvoid f(va_list ap) { (void)va_arg(ap, long double); }",
    "range": "long double x = 0x1p16384L;",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    if platform.machine().lower() not in ("amd64", "x86_64"):
        print("SysV AMD64 reference execution requires an x86-64 host")
        return
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-amd64-long-double-") as directory:
        work = Path(directory)
        env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        env.pop("CEPHYR_CONFIG", None)
        cephyr = (str(args.cephyr.resolve()), "-std=c11", "-I", HEADERS)
        amd64 = "--target=x86_64-linux-gnu"

        def run(*command, expected=None):
            result = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True)
            success = (result.returncode == 0 if expected is None else
                       result.returncode != 0 and expected in result.stderr)
            if not success:
                raise AssertionError((command, result.returncode, result.stdout, result.stderr))

        (work / "native.c").write_text(NATIVE, encoding="ascii")
        (work / "external.c").write_text(EXTERNAL, encoding="ascii")
        (work / "reference.c").write_text(DECLARATIONS + RUNTIME, encoding="ascii")
        reference_names = ("transfer", "discard", "calculate", "predicates", "widen",
                           "narrow_values", "narrow_ints", "conditional_effects")
        run(args.cc, "-std=c11", "-O2", "-frounding-math", "-ffp-contract=off",
            *(f"-D{name}=reference_{name}" for name in reference_names),
            "-c", "reference.c", "-o", "reference.o")
        for options in ((), ("-fPIC", "--qml-pass=canonicalize", "--qml-pass=strength_reduce")):
            run(*cephyr, amd64, *options, "--emit-mir", "native.c", "-o", "native.mir")
            mir = (work / "native.mir").read_text()
            # An unused volatile value still reads each payload chunk.
            discard = mir.split("function discard ", 1)[1].split("\nret", 1)[0]
            assert discard.count("\nload ") == 1 and discard.count("\nload_u16 ") == 1, discard
            opcodes = ("add", "sub", "mul", "div", "eq", "lt", "le",
                       "from_fp", "from_si", "from_ui", "to_fp", "to_si", "to_ui")
            assert all(f"\namd64_x87_{op} " in mir for op in opcodes), mir
            run(*cephyr, amd64, *options, "-c", "native.c", "-o", "native.o")
            run(args.cc, "-std=c11", "-O2", "external.c", "native.o", "reference.o", "-lm", "-o", "test")
            run("./test")
        expected = {"range": "out of range", "const_assignment": "modifiable",
                    "remainder": "operator does not accept"}
        for name, source in NEGATIVE.items():
            filename = f"{name}.c"
            (work / filename).write_text(source + "\n", encoding="ascii")
            run(*cephyr, amd64, "-S", filename, "-o", "rejected.s",
                expected=expected.get(name, "long double"))
            assert not (work / "rejected.s").exists()
        # The identical spelling requires a different, unsupported quad
        # representation on AArch64 and RV64; do not give it x87 bytes.
        for triple in ("aarch64-linux-gnu", "riscv64-linux-gnu"):
            (work / "other.c").write_text("long double x = 1.L;\n", encoding="ascii")
            run(*cephyr, f"--target={triple}", "-S", "other.c", "-o", "rejected.s",
                expected="long double")


if __name__ == "__main__":
    main()
