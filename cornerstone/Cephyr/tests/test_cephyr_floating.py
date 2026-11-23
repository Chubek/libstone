"""End-to-end native scalar FP: independent C ABI caller and RV64 arithmetic refusal.

Run with a Cephyr executable after the frontend FP wiring is enabled; this test
does not use a host compiler to produce any of the code under test.
"""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "floating"
START = {
    "x86_64": ("call test_main", "movl %eax, %edi", "movl $60, %eax", "syscall"),
    "aarch64": ("bl test_main", "mov x8, #93", "svc #0"),
}
EMULATIONS = {"x86_64": "elf_x86_64", "aarch64": "aarch64elf"}
NEGATIVE = {
    "shift": "float f(float x) { return x << 1; }\n",
    "bitwise": "float f(float x) { return ~x; }\n",
    "remainder": "float f(float x) { return x % 1; }\n",
    "pointer-cast": "void *f(float x) { return (void *)x; }\n",
    "function-pointer-cast": "int (*f(float x))(void) { return (int (*)(void))x; }\n",
    "bad-static": "double g(void); static double x = g();\n",
    "overflow-literal": "double f(void) { return 1e9999999; }\n",
    "overflow-object": "static float x = 0x1p128;\n",
}


def run(*args, work, env, expected=None):
    process = subprocess.run(args, cwd=work, env=env, capture_output=True, text=True, timeout=300)
    if expected:
        if process.returncode <= 0 or expected not in process.stderr:
            raise AssertionError((args, expected, process.returncode, process.stderr))
    elif process.returncode != 0:
        raise AssertionError((args, process.returncode, process.stdout, process.stderr))
    return process.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--lld", default="ld.lld")
    parser.add_argument("--qemu-aarch64", default=shutil.which("qemu-aarch64") or
                        shutil.which("qemu-aarch64-static"))
    args = parser.parse_args()
    host = platform.machine().lower()
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-floating-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))

        def check(*command, expected=None):
            return run(*command, work=work, env=env, expected=expected)

        base = (str(args.cephyr.resolve()), "-std=c11")
        source = FIXTURES / "native.c"
        for arch in START:
            triple = f"{arch}-linux-gnu"
            cephyr = (*base, f"--target={triple}")
            clang = (args.clang, f"--target={triple}", "-ffreestanding", "-fno-pie",
                     "-fno-stack-protector", "-ffp-contract=off", "-O2", "-c")
            mir = work / f"{arch}.mir"
            check(*cephyr, "--emit-mir", source, "-o", mir)
            words = mir.read_text(encoding="ascii").split()
            for expected in ("f32", "f64", "fconst", "fload", "fstore", "fneg",
                             "fadd", "fsub", "fmul", "fdiv", "feq", "fle", "fcast",
                             "sitofp", "uitofp", "fptosi", "fptoui", "call_indirect"):
                if expected not in words:
                    raise AssertionError(f"{triple}: MachineIR is missing {expected}")

            native, harness, entry, exe = (work / f"{arch}.{suffix}" for suffix in
                                           ("cephyr.o", "harness.o", "start.o", "elf"))
            check(*cephyr, "-c", source, "-o", native)
            check(*clang, FIXTURES / "harness.c", "-o", harness)
            start = work / "start.s"
            start.write_text("\n".join((".text", ".globl _start", "_start:", *START[arch],
                                       '.section .note.GNU-stack,"",%progbits', "")), encoding="ascii")
            check(args.clang, f"--target={triple}", "-c", start, "-o", entry)
            check(args.lld, "-m", EMULATIONS[arch], "-static", entry, native, harness, "-o", exe)
            if host in (("x86_64", "amd64") if arch == "x86_64" else ("aarch64", "arm64")):
                check(exe)
            elif arch == "aarch64" and args.qemu_aarch64:
                check(args.qemu_aarch64, exe)
            else:
                print(f"{triple}: linked; execution skipped (no emulator)")

        # RV64IM lowers runtime arithmetic to typed, integer-ABI helper
        # calls rather than issuing hardware FP instructions.
        rv = (*base, "--target=riscv64-linux-gnu")
        rv_source = work / "rv64.c"
        rv_source.write_text(
            '#ifdef __CEPHYR_HAS_SCALAR_FP\n#error "unexpected hard FP macro"\n#endif\n'
            "float f(float x) { return x + 0.25f; }\n", encoding="ascii")
        rv_mir = work / "rv64.mir"
        check(*rv, "--emit-mir", rv_source, "-o", rv_mir)
        words = rv_mir.read_text(encoding="ascii").split()
        if "__addsf3" not in words or "fadd" in words:
            raise AssertionError("RV64 float arithmetic did not lower to a soft-float helper")
        rv_int_cast = work / "rv64-ice.c"
        rv_int_cast.write_text(
            "_Static_assert((int)0x1.8p1 == 3, \"integer constant expression\");\n"
            "int f(void) { return (int)0x1.8p1; }\n", encoding="ascii")
        rv_ice_mir = work / "rv64-ice.mir"
        check(*rv, "--emit-mir", rv_int_cast, "-o", rv_ice_mir)
        if any(op in rv_ice_mir.read_text(encoding="ascii").split() for op in ("f32", "f64", "fconst")):
            raise AssertionError("RV64 folded integer literal casts must not introduce FP MIR")

        amd = (*base, "--target=x86_64-linux-gnu")
        for name, text in NEGATIVE.items():
            src = work / f"bad-{name}.c"
            out = work / f"bad-{name}.s"
            src.write_text(text, encoding="ascii")
            check(*amd, "-S", src, "-o", out, expected="error")
            if out.exists():
                raise AssertionError(f"{name}: unexpectedly published {out}")


if __name__ == "__main__":
    main()
