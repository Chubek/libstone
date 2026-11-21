"""Exercise Cephyr's AAPCS64 variadic ABI against Clang, without a target libc."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


CEPHYR = r"""
#include <stdarg.h>
#ifndef __CEPHYR_HAS_AARCH64_STDARG
#error "missing AAPCS64 stdarg capability"
#endif
_Static_assert(sizeof(va_list) == 32, "AAPCS64 va_list size");
_Static_assert(_Alignof(va_list) == 8, "AAPCS64 va_list alignment");
typedef int (*callback)(int);

int clang_consume(va_list *ap);
double clang_receive(float fixed, int tag, ...);
int clang_receive_overflow(int tag, ...);
int clang_disturb(double fp, int gp);

int cephyr_consume(va_list *ap) {
    return va_arg(*ap, double) != 42.5 || va_arg(*ap, int) != -17;
}

int cephyr_receive(float fixed, int tag, ...) {
    va_list ap[2], copy;
    int n = clang_disturb(7.0, 8);  // Clobber incoming regs before va_start.
    int i = 0;
    tag = 100;                    // va_start must not inspect the value.
    va_start(ap[i++], tag);
    if (n != 15 || i != 1 || fixed != 0.5f || tag != 100) return 1;
    if (sizeof va_arg(ap[0], int) != sizeof(int) ||
        _Generic(va_arg(ap[0], double), double: 0, default: 1)) return 2;
    i = 0;
    if (va_arg(ap[i++], int) != -11 || i != 1) return 3;
    va_copy(copy, ap[0]);
    if (va_arg(copy, double) != 1.25) return 4;
    va_end(copy);
    if (va_arg(ap[0], double) != 1.25) return 5;
    // Clang advances our list in-place, exhausting both register banks and
    // traversing interleaved stack slots. Cephyr resumes from its cursor.
    if (clang_consume(&ap[0])) return 6;
    if (*va_arg(ap[0], const int *) != 31 || va_arg(ap[0], callback)(12) != 16)
        return 7;
    va_end(ap[0]);
    va_start(ap[0], tag);
    if (va_arg(ap[0], int) != -11) return 8;
    va_end(ap[0]);
    return 0;
}

int cephyr_named_overflow(int a, int b, int c, int d, int e, int f, int g,
                          int h, int i, double j, double k, double l, double m,
                          double n, double o, double p, double q, double r, ...) {
    va_list ap;
    va_start(ap, r);
    unsigned x = va_arg(ap, unsigned);
    double y = va_arg(ap, double);
    long z = va_arg(ap, long);
    va_end(ap);
    return i != 9 || r != 9.25 || x != 4000000000U || y != -1.5 || z != 0x123456789L;
}

static double cephyr_local(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    double fp = va_arg(ap, double);
    int gp = va_arg(ap, int);
    va_end(ap);
    return fp + gp + tag;
}

int cephyr_send(void) {
    if (cephyr_local(0, 0.75f, (signed char)-4) != -3.25) return 1;
    if (clang_receive(0.5f, -2, 1.25f, (unsigned short)65280) != 65279.75) return 2;
    return clang_receive_overflow(0, 42.5, -17, 1, 1.0, 2, 2.0, 3, 3.0, 4, 4.0,
                                  5, 5.0, 6, 6.0, 7, 7.0, 8, 8.0, 9, 9.0);
}
"""


CLANG = r"""
#include <stdarg.h>
_Static_assert(sizeof(va_list) == 32 && _Alignof(va_list) == 8, "AAPCS64 layout");
int cephyr_receive(float, int, ...);
int cephyr_named_overflow(int, int, int, int, int, int, int, int, int,
                          double, double, double, double, double,
                          double, double, double, double, ...);
int cephyr_send(void);
int cephyr_consume(va_list *);

__attribute__((naked)) int clang_disturb(double fp, int gp) {
    __asm__("mov x1, #0; mov x2, #0; fmov d1, xzr; fmov d2, xzr; mov w0, #15; ret");
}

int clang_consume(va_list *ap) {
    for (int n = 2; n < 10; ++n) {
        if (va_arg(*ap, int) != n + 20 || va_arg(*ap, double) != (double)n)
            return 1;
    }
    return 0;
}

double clang_receive(float fixed, int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    double fp = va_arg(ap, double);
    int gp = va_arg(ap, int);
    va_end(ap);
    return fixed + tag + fp + gp;
}

int clang_receive_overflow(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    int result = cephyr_consume(&ap);
    for (int n = 1; !result && n <= 9; ++n) {
        if (va_arg(ap, int) != n || va_arg(ap, double) != (double)n)
            result = 1;
    }
    va_end(ap);
    return result;
}

static int add_four(int n) { return n + 4; }

static int test(void) {
    const int object = 31;
    int result = cephyr_receive(0.5f, 10, -11, 1.25, 22, 2.0, 23, 3.0, 24, 4.0,
                                25, 5.0, 26, 6.0, 27, 7.0, 28, 8.0, 29, 9.0,
                                &object, add_four);
    if (result) return result;
    if (cephyr_named_overflow(1, 2, 3, 4, 5, 6, 7, 8, 9, 1.25, 2.25, 3.25, 4.25,
                               5.25, 6.25, 7.25, 8.25, 9.25, 4000000000U, -1.5,
                               0x123456789L)) return 9;
    return cephyr_send() ? 10 : 0;
}

void _start(void) {
    register long result __asm__("x0") = test();
    register long syscall __asm__("x8") = 93;
    __asm__ volatile("svc #0" :: "r"(result), "r"(syscall) : "memory");
    __builtin_unreachable();
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--qemu", default=os.environ.get("CEPHYR_TEST_QEMU_AARCH64") or
                        shutil.which("qemu-aarch64") or shutil.which("qemu-aarch64-static"))
    args = parser.parse_args()
    target = "--target=aarch64-linux-gnu"
    cephyr = [args.cephyr.resolve(), target, "-std=c11"]
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-aarch64-stdarg-") as directory:
        work = Path(directory)
        env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        env.pop("CEPHYR_CONFIG", None)

        def run(command, **kwargs):
            return subprocess.run(command, cwd=work, env=env, **kwargs)

        rejects = (
            ("void f(int x) { va_list a; va_start(a, x); }", "rightmost eligible"),
            ("void f(int x, int y, ...) { va_list a; va_start(a, x); }", "rightmost eligible"),
            ("void f(register int x, ...) { va_list a; va_start(a, x); }", "rightmost eligible"),
            ("void f(float x, ...) { va_list a; va_start(a, x); }", "rightmost eligible"),
            ("void f(int x, ...) { const va_list a; va_start(a, x); }", "writable AAPCS64"),
            ("void f(int x, ...) { va_list a; va_arg(a, float); }", "only promoted"),
            ("void f(int x, ...) { va_list a; va_arg(a, long double); }", "long double"),
            ("struct S { int i; }; void f(int x, ...) { va_list a; va_arg(a, struct S); }",
             "only promoted"),
            ("void f(va_list a) { }", "by-value aggregate ABI"),
            ("struct S { int i; }; void f(struct S x, ...) { }", "fixed prototype"),
            ("struct S { int i; }; int g(int, ...); int f(void) { struct S a = {1}; return g(0, a); }",
             "variadic argument must be an integer, pointer or supported floating type"),
            ("int f(int (*g)(int, ...)) { return g(0, 1); }", "variadic indirect"),
        )
        for source, diagnostic in rejects:
            (work / "reject.c").write_text("#include <stdarg.h>\n" + source + "\n", encoding="ascii")
            result = run([*cephyr, "-fsyntax-only", "reject.c"], capture_output=True, text=True)
            if result.returncode <= 0 or diagnostic not in result.stderr:
                raise AssertionError(f"expected {diagnostic!r}\n{result.stderr}")

        (work / "native.c").write_text(CEPHYR, encoding="ascii")
        (work / "external.c").write_text(CLANG, encoding="ascii")
        clang = [args.clang, target]
        can_link = shutil.which(args.clang) and shutil.which("ld.lld")
        if can_link:
            run([*clang, "-std=c11", "-O2", "-ffreestanding", "-fno-stack-protector", "-c", "external.c",
                 "-o", "external.o"], check=True)
        host = platform.system() == "Linux" and platform.machine().lower() in ("aarch64", "arm64")
        for mode, options in (("plain", ()), ("qml", ("--qml-pass=canonicalize",))):
            obj = f"native-{mode}.o"
            run([*cephyr, *options, "-c", "native.c", "-o", obj], check=True)
            if can_link:
                exe = f"check-{mode}"
                run([*clang, "-fuse-ld=lld", "-nostdlib", "-static", "external.o", obj,
                     "-o", exe], check=True)
                if host or args.qemu:
                    run([work / exe] if host else [args.qemu, work / exe], check=True)
        if not can_link or not (host or args.qemu):
            print("AAPCS64 objects compiled; cross-ABI execution requires Clang/lld and QEMU or AArch64 Linux")


if __name__ == "__main__":
    main()
