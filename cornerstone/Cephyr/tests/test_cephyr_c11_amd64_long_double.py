"""Verify Cephyr's SysV AMD64 x87 storage against an external C compiler."""

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
""".replace("VALUES", VALUES)
EXTERNAL = DECLARATIONS + r"""
#include <stdint.h>
#include <string.h>
extern const long double table[];
extern const struct Packet packet;
extern const union Slot slot;
extern long double zeroed[];
extern unsigned long long wide;
extern double narrow;
extern float single;
extern int probe(void);

int consume(const volatile long double *p, const struct Packet *s) {
    if ((uintptr_t)p % 16)
        return 105;
    if (*p != 1.L / 3.L)
        return 106;
    if (s->tag != 7 || s->value != 0x1.0000000000000002p0L || s->end != 0xabcd)
        return 107;
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
    return probe();
}
""".replace("VALUES", VALUES)

NEGATIVE = {
    "return": "long double f(void) { return 1.L; }",
    "parameter": "int f(long double x) { return 1; }",
    "call": "extern int g(long double); int f(void) { return g(1.L); }",
    "indirect": "int f(int (*g)(long double)) { return g(1.L); }",
    "load": "long double x; int f(void) { return (int)x; }",
    "assign": "long double x; int f(double y) { x = y; return 0; }",
    "convert": "int f(double x) { return (int)(long double)x; }",
    "initializer": "int f(double y) { long double x = y; return 0; }",
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
        for options in ((), ("--qml-pass=canonicalize",)):
            run(*cephyr, amd64, *options, "-c", "native.c", "-o", "native.o")
            run(args.cc, "-std=c11", "-O2", "external.c", "native.o", "-o", "test")
            run("./test")
        for name, source in NEGATIVE.items():
            filename = f"{name}.c"
            (work / filename).write_text(source + "\n", encoding="ascii")
            run(*cephyr, amd64, "-S", filename, "-o", "rejected.s",
                expected="out of range" if name == "range" else "long double")
            assert not (work / "rejected.s").exists()
        # The identical spelling requires a different, unsupported quad
        # representation on AArch64 and RV64; do not give it x87 bytes.
        for triple in ("aarch64-linux-gnu", "riscv64-linux-gnu"):
            (work / "other.c").write_text("long double x = 1.L;\n", encoding="ascii")
            run(*cephyr, f"--target={triple}", "-S", "other.c", "-o", "rejected.s",
                expected="long double")


if __name__ == "__main__":
    main()
