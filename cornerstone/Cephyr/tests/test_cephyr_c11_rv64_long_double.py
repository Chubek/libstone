"""GCC/Cephyr mixed binary128 calls on an integer-only RV64IM CPU."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

from test_cephyr_c11_aarch64_long_double import NATIVE, EXTERNAL

# The common fixture covers a fully stacked, aligned pair after the ninth
# scalar. Also force a7/stack splitting, odd fixed register pairs and the
# even-register rule on the variadic side of the fixed/variable boundary.
PARAMS = "long a,long b,long c,long d,long e,long f,long g,long double q,long double r,long h"
NATIVE_RV = """
extern long double external_split(PARAMS);
long double forward_split(PARAMS) { return external_split(a,b,c,d,e,f,g,q,r,h); }
struct Pair { long x,y; };
extern struct Pair external_pair(PARAMS);
struct Pair forward_pair(PARAMS) { return external_pair(a,b,c,d,e,f,g,q,r,h); }
extern long double external_va(long n,...);
long double call_va(long double q) {
    return external_va(6,1L,2L,3L,4L,5L,6L,q,9L,q);
}
""".replace("PARAMS", PARAMS)
EXTERNAL_RV = """
#include <stdarg.h>
extern long double forward_split(PARAMS);
extern long double call_va(long double);
struct Pair { long x,y; };
extern struct Pair forward_pair(PARAMS);
struct Pair external_pair(PARAMS) {
    struct Pair p = {q == 1.L + LDBL_EPSILON && r == 1.L ? a+b+c+d+e+f+g : 0, h};
    return p;
}
long double external_split(PARAMS) {
    return a+b+c+d+e+f+g == 28 && h == 9 ? q-r : 0;
}
long double external_va(long n,...) {
    va_list ap; va_start(ap,n);
    for (long i=1; i<=n; ++i) if (va_arg(ap,long)!=i) return 0;
    long double first=va_arg(ap,long double);
    if (va_arg(ap,long)!=9) return 0;
    long double second=va_arg(ap,long double);
    va_end(ap);
    return first == second ? first : 0;
}
int main(void) {
    long double q=0x1.0000000000000000000000000001p0L;
    if (forward_split(1,2,3,4,5,6,7,q,1.L,9) != LDBL_EPSILON) return 9;
    if (call_va(q) != q) return 10;
    struct Pair p = forward_pair(1,2,3,4,5,6,7,q,1.L,9);
    if (p.x != 28 || p.y != 9) return 11;
    return common_tests();
}
""".replace("PARAMS", PARAMS)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--cc", default=os.environ.get("CEPHYR_TEST_CC_RV64") or "riscv64-linux-gnu-gcc")
    parser.add_argument("--qemu", default=shutil.which("qemu-riscv64") or shutil.which("qemu-riscv64-static"))
    args = parser.parse_args()
    target = "--target=riscv64-linux-gnu"
    headers = Path(__file__).resolve().parents[1] / "stdlib/include"
    with tempfile.TemporaryDirectory(prefix="cephyr-rv64-quad-") as tmp:
        work = Path(tmp)
        env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        def run(*cmd):
            subprocess.run(cmd, cwd=work, env=env, check=True)
        (work / "native.c").write_text(NATIVE + NATIVE_RV)
        (work / "external.c").write_text(EXTERNAL.replace("int main(void)", "int common_tests(void)") + EXTERNAL_RV)
        cc = (args.cc, "-march=rv64im", "-mabi=lp64", "-mno-relax", "-fno-pie", "-fno-pic")
        run(*cc, "-std=c11", "-O2", "-ffreestanding", "-fno-stack-protector", "-I", headers,
            "-c", "external.c", "-o", "external.o")
        run(args.cephyr.resolve(), target, "--qml-pass=canonicalize", "-S", "native.c", "-o", "native.s")
        run(*cc, "-c", "native.s", "-o", "native.o")
        run(args.cephyr.resolve(), target, "--stdlib=cephyr", "native.o", "external.o", "-o", "check")
        if args.qemu:
            run(args.qemu, "-cpu", "rv64i,m=true", "./check")
        elif platform.machine().lower() == "riscv64":
            run("./check")
        else:
            print("RV64IM binary128 linked; execution skipped (no QEMU)")


if __name__ == "__main__":
    main()
