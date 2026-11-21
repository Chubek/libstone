"""Check Cephyr's native SysV AMD64 variadic ABI against the host C compiler."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


NATIVE = r"""
#include <stdarg.h>
#if !defined(__CEPHYR_HAS_AMD64_STDARG)
#error "missing native stdarg capability"
#endif
_Static_assert(sizeof(va_list) == 24, "SysV va_list size");
_Static_assert(_Alignof(va_list) == 8, "SysV va_list alignment");

typedef int (*callback)(int);
extern int check_forward(va_list ap);
extern double other_side(float fixed, int tag, ...);

static int disturbance(int a, int b, int c, int d, int e, int f,
                       int g, int h, double i) {
    return a + b + c + d + e + f + g + h + (int)i;
}
static int pull(va_list *ap) { return va_arg(*ap, int); }

int cephyr_receive(int tag, ...) {
    va_list ap, copy;
    va_list *cursor = &ap;
    // An actual integer-register/FP-register and stack call before va_start
    // ensures that the initial register contents survive an intervening call.
    int result = disturbance(1, 2, 3, 4, 5, 6, 7, 8, 9.0);
    tag += 10; // va_start identifies the named argument without reading it.
    va_start(ap, tag);
    if (sizeof va_arg(ap, long) != sizeof(long))
        return 1;
    if (pull(cursor) != -11)
        return 2;
    va_copy(copy, ap);
    if (va_arg(copy, double) != 1.25)
        return 3;
    va_end(copy);
    if (_Generic(va_arg(ap, double), double: 1, default: 0) != 1)
        return 4;
    if (va_arg(ap, double) != 1.25 || va_arg(ap, long) != 0x100000007L)
        return 5;
    if (*va_arg(ap, const int *) != 31 || va_arg(ap, callback)(12) != 16)
        return 6;
    if (va_arg(ap, unsigned) != 4000000000U)
        return 7;
    if (va_arg(ap, int) != -22 || va_arg(ap, double) != 2.5)
        return 8;
    va_end(ap);
    return result != 45 ? 9 : 0;
}

int cephyr_forward(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    int result = check_forward(ap);
    va_end(ap);
    return result;
}

int cephyr_named_overflow(int a, int b, int c, int d, int e, int f, int g,
                          double p, double q, double r, double s, double t,
                          double u, double v, double w, double x, ...) {
    va_list ap;
    va_start(ap, x);
    int first = va_arg(ap, int);
    double second = va_arg(ap, double);
    va_end(ap);
    return g != 7 || x != 9.5 || first != -17 || second != 10.25;
}

double cephyr_named_fp(float a, int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    double d = va_arg(ap, double);
    int i = va_arg(ap, int);
    va_end(ap);
    return a + tag + d + i;
}

static double cephyr_local(int tag, ...) {
    va_list lists[2];
    int index = 0;
    va_start(lists[index++], tag);
    if (index != 1)
        return -1.0;
    index = 0;
    double real = va_arg(lists[index++], double);
    if (index != 1)
        return -2.0;
    int promoted = va_arg(lists[0], int);
    va_end(lists[0]);
    return real + promoted;
}

int cephyr_send(void) {
    if (cephyr_local(0, 0.75f, (signed char)-4) != -3.25)
        return 1;
    return other_side(0.5f, 0, 1.25f, (unsigned short)65280) != 65281.75;
}
"""


EXTERNAL = r"""
#include <stdarg.h>
int cephyr_receive(int, ...);
int cephyr_forward(int, ...);
int cephyr_named_overflow(int, int, int, int, int, int, int,
                          double, double, double, double, double,
                          double, double, double, double, ...);
double cephyr_named_fp(float, int, ...);
int cephyr_send(void);

static int add_four(int i) { return i + 4; }

int check_forward(va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int i = va_arg(copy, int);
    double d = 0.0;
    for (int n = 1; n <= 9; ++n)
        d += va_arg(copy, double) * n;
    long l = va_arg(copy, long);
    va_end(copy);
    return i == -11 && d == 285.0 && l == 0x123456789L ? 0 : 10;
}

double other_side(float fixed, int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    double first = va_arg(ap, double);
    int second = va_arg(ap, int);
    va_end(ap);
    return fixed + first + second;
}

int main(void) {
    const int object = 31;
    int n = cephyr_receive(0, (signed char)-11, 1.25, 0x100000007L,
                            &object, add_four, 4000000000U, -22, 2.5);
    if (n)
        return n;
    n = cephyr_forward(0, -11, 1.0, 2.0, 3.0, 4.0, 5.0,
                         6.0, 7.0, 8.0, 9.0, 0x123456789L);
    if (n)
        return n;
    if (cephyr_named_overflow(1, 2, 3, 4, 5, 6, 7, 1.5, 2.5, 3.5, 4.5,
                               5.5, 6.5, 7.5, 8.5, 9.5, -17, 10.25))
        return 11;
    if (cephyr_named_fp(0.5f, 10, 2.75, -3) != 10.25)
        return 13;
    return cephyr_send() ? 12 : 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-amd64-stdarg-") as directory:
        work = Path(directory)
        env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        env.pop("CEPHYR_CONFIG", None)
        cephyr = [args.cephyr.resolve(), "-std=c11"]

        def run(command, **kwargs):
            return subprocess.run(command, cwd=work, env=env, **kwargs)

        def reject(triple, source, diagnostic):
            (work / "reject.c").write_text(source, encoding="ascii")
            result = run([*cephyr, f"--target={triple}", "-fsyntax-only", "reject.c"],
                         capture_output=True, text=True)
            if result.returncode <= 0 or diagnostic not in result.stderr:
                raise AssertionError(f"{triple}: expected {diagnostic!r}\n{result.stderr}")

        amd64 = "x86_64-linux-gnu"
        reject(amd64, "#include <stdarg.h>\nvoid f(int x) { va_list a; va_start(a, x); }",
               "rightmost eligible named parameter")
        reject(amd64, "#include <stdarg.h>\nvoid f(int x, int y, ...) { va_list a; va_start(a, x); }",
               "rightmost eligible named parameter")
        reject(amd64, "#include <stdarg.h>\nvoid f(register int x, ...) { va_list a; va_start(a, x); }",
               "rightmost eligible named parameter")
        reject(amd64, "#include <stdarg.h>\nvoid f(float x, ...) { va_list a; va_start(a, x); }",
               "rightmost eligible named parameter")
        reject(amd64, "#include <stdarg.h>\nvoid f(int x, ...) { va_list a; va_arg(a, float); }",
               "only promoted integer, pointer and double")
        reject(amd64, "#include <stdarg.h>\nstruct S { int field; };\n"
               "void f(int x, ...) { va_list a; va_arg(a, struct S); }",
               "only promoted integer, pointer and double")
        (work / "native.c").write_text(NATIVE, encoding="ascii")
        (work / "external.c").write_text(EXTERNAL, encoding="ascii")
        host = platform.system() == "Linux" and platform.machine().lower() in ("x86_64", "amd64")
        for mode, options in (("plain", ()), ("qml", ("--qml-pass=canonicalize",))):
            obj = f"native-{mode}.o"
            run([*cephyr, f"--target={amd64}", *options, "-c", "native.c", "-o", obj], check=True)
            if host:
                run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                     "external.c", obj, "-o", f"check-{mode}"], check=True)
                run([work / f"check-{mode}"], check=True)
        if not host:
            print("SysV AMD64 objects compiled; host ABI execution skipped")


if __name__ == "__main__":
    main()
