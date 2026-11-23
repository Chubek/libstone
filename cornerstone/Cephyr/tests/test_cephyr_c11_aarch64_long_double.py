"""Check binary128 memory, calls and return values across GCC/AAPCS64."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


NATIVE = r"""
#include <float.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
_Static_assert(sizeof(long double) == 16 && _Alignof(long double) == 16, "object");
_Static_assert((unsigned long long)0x1.fffffffffffffffep63L == ~0ULL, "integer cast");
long double table[] = { LDBL_MAX, LDBL_TRUE_MIN, -0.L, 1.0000000000000000000000000000000002L,
                        (1.L + LDBL_EPSILON) - 1.L, 1.L / 3.L };
long double parsed(void) {
    wchar_t *w; char *c;
    long double a = strtold("0x1.0000000000000000000000000001p0!", &c);
    long double b = wcstold(L"0x1p-16494?", &w);
    return *c == '!' && *w == '?' && b == LDBL_TRUE_MIN ? a : 0.L;
}
long double receive(int n, ...) {
    va_list ap;
    va_start(ap,n);
    long double result = 0.L;
    for (int i = 0; i < n; ++i) result = va_arg(ap,long double);
    va_end(ap);
    return result;
}
extern long double external_var(int,...);
long double send(void) { return external_var(9,0.,1.,2.,3.,4.,5.,6.,7.,8.,table[3]); }
typedef long double (*fn)(double,long double);
long double callback(fn f, double d, long double x) { return f(d,x); }
long double compute(volatile long double *p, unsigned long long n, float f, double d) {
    long double x = (*p + n) * f / d;
    *p = -x;
    return x - n;
}
int narrow(long double x) {
    return (int)x == -2 && (unsigned long long)(-x) == 2 && (double)x == -2.5 && (float)x == -2.5f;
}
int predicates(long double x, long double y) {
    return (!x) + 2*(x != y) + 4*(x < y) + 8*(x <= y) + 16*(x == y) + 32*(x >= y) + 64*(x > y);
}
#define MANY double a,double b,double c,double d,double e,double f,double g,double h,double i,long double x,long double y
extern long double external_many(MANY);
long double forward(MANY) { return external_many(a,b,c,d,e,f,g,h,i,x,y); }
"""

EXTERNAL = r"""
#include <float.h>
#include <string.h>
#include <stdarg.h>
extern long double parsed(void),receive(int,...),table[];
extern long double send(void);
long double external_var(int n,...) {
    va_list ap;
    va_start(ap,n);
    for (int i=0;i<n;++i) if (va_arg(ap,double)!=i) return 0;
    long double x = va_arg(ap,long double);
    va_end(ap);
    return x;
}
extern long double callback(long double (*)(double,long double),double,long double);
extern long double compute(volatile long double *,unsigned long long,float,double);
extern int narrow(long double), predicates(long double,long double);
#define MANY double a,double b,double c,double d,double e,double f,double g,double h,double i,long double x,long double y
extern long double forward(MANY);
long double external_many(MANY) {
    return a==0 && b==1 && c==2 && d==3 && e==4 && f==5 && g==6 && h==7 && i==8 ? x-y : 0;
}
static long double add(double d,long double x) { return d+x; }
int main(void) {
    const long double reference[] = {LDBL_MAX,LDBL_TRUE_MIN,-0.L,1.0000000000000000000000000000000002L,
                                     LDBL_EPSILON,1.L/3.L};
    if (memcmp(table,reference,sizeof reference)) return 1;
    if (parsed() != 1.L + LDBL_EPSILON) return 2;
    if (callback(add,-1.,table[3]) != LDBL_EPSILON) return 3;
    if (forward(0,1,2,3,4,5,6,7,8,table[3],1.L) != LDBL_EPSILON) return 4;
    if (receive(9,0.L,1.L,2.L,3.L,4.L,5.L,6.L,7.L,table[3]) != table[3]) return 5;
    if (send() != table[3]) return 9;
    volatile long double x = 1.L;
    if (compute(&x,~0ULL,1.5f,3.) != -(0x1p63L-1.L) || x != -0x1p63L) return 6;
    if (!narrow(-2.5L)) return 7;
    if (predicates(__builtin_nanl(""), 1.L) != 2 || predicates(-0.L,0.L) != 57 ||
        predicates(-1.L,1.L) != 14 || predicates(1.L,-1.L) != 98) return 8;
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--cc", default=os.environ.get("CEPHYR_TEST_CC_AARCH64") or "aarch64-linux-gnu-gcc")
    parser.add_argument("--qemu", default=os.environ.get("CEPHYR_TEST_QEMU_AARCH64") or
                        shutil.which("qemu-aarch64") or shutil.which("qemu-aarch64-static"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cephyr-aapcs64-quad-") as tmp:
        work = Path(tmp)
        env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        target = "--target=aarch64-linux-gnu"
        headers = Path(__file__).resolve().parents[1] / "stdlib/include"
        (work / "external.c").write_text(EXTERNAL)
        (work / "native.c").write_text(NATIVE)
        def run(*cmd):
            subprocess.run(cmd, cwd=work, env=env, check=True)
        run(args.cc, "-std=c11", "-O2", "-ffreestanding", "-fno-pic", "-fno-pie", "-fno-stack-protector", "-I", headers,
            "-c", "external.c", "-o", "external.o")
        run(args.cephyr.resolve(), target, "--qml-pass=canonicalize", "native.c", "external.o", "-o", "check")
        if platform.machine().lower() in ("aarch64", "arm64"):
            run("./check")
        elif args.qemu:
            run(args.qemu, "./check")
        else:
            print("AAPCS64 binary128 linked; execution skipped (no QEMU)")


if __name__ == "__main__":
    main()
