"""RV64IM LP64 soft-float storage and fixed-prototype ABI against Clang."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import tempfile


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "rv64_soft_fp"
TRIPLE = "riscv64-linux-gnu"
NEGATIVE = (
    ("float f(float x, float y) { return x + y; }", "runtime floating-point arithmetic"),
    ("double f(double x, double y) { return x / y; }", "runtime floating-point arithmetic"),
    ("double f(float x) { return x; }", "runtime floating-point conversions"),
    ("float f(double x) { return x; }", "runtime floating-point conversions"),
    ("double f(unsigned long x) { return x; }", "runtime floating-point conversions"),
    ("int f(float x) { return x; }", "runtime floating-point conversions"),
    ("int g(int, ...); int f(float x) { return g(1, x); }", "floating-point variadic calls"),
    ("long double f(long double x) { return x; }", "long double"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--lld", default="ld.lld")
    parser.add_argument("--qemu", default=shutil.which("qemu-riscv64") or
                        shutil.which("qemu-riscv64-static"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-rv64-soft-fp-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))

        def check(*command, expected=None):
            p = subprocess.run(command, cwd=work, env=env, capture_output=True,
                               text=True, timeout=300)
            if expected is not None:
                if p.returncode <= 0 or expected not in p.stderr:
                    raise AssertionError((command, expected, p.returncode, p.stdout, p.stderr))
            elif p.returncode != 0:
                raise AssertionError((command, p.returncode, p.stdout, p.stderr))

        cephyr = (str(args.cephyr.resolve()), "-std=c11", f"--target={TRIPLE}")
        clang = (args.clang, f"--target={TRIPLE}", "-march=rv64im", "-mabi=lp64")
        asm, native, harness, entry, exe = (work / name for name in
                                             ("native.s", "native.o", "harness.o", "start.o", "test"))
        start = work / "start.s"
        start.write_text(".text\n.globl _start\n_start:\ncall test_main\nli a7,93\necall\n",
                         encoding="ascii")
        check(*clang, "-c", start, "-o", entry)
        check(*clang, "-std=c11", "-O2", "-ffreestanding", "-fno-pie", "-fno-stack-protector",
              "-c", FIXTURES / "harness.c", "-o", harness)
        for options in ((), ("--qml-pass=canonicalize",)):
            check(*cephyr, *options, "-S", FIXTURES / "native.c", "-o", asm)
            # Assembly must be accepted by a strict RV64IM assembler.
            check(*clang, "-c", asm, "-o", native)
            check(*cephyr, *options, "-c", FIXTURES / "native.c", "-o", native)
            # ELF64 little-endian e_flags: RVC and floating ABI bits must be clear.
            (flags,) = struct.unpack_from("<I", native.read_bytes(), 48)
            if flags & 7:
                raise AssertionError(f"unexpected RV64IM LP64 flags: {flags:#x}")
            check(args.lld, "-m", "elf64lriscv", "-static", entry, native, harness, "-o", exe)
            if args.qemu:
                check(args.qemu, "-cpu", "rv64i,m=true", exe)
            elif platform.machine().lower() == "riscv64":
                check(exe)
            else:
                print(f"{TRIPLE}: linked; execution skipped (no emulator)")
        for i, (text, diagnostic) in enumerate(NEGATIVE):
            source, output = work / f"bad-{i}.c", work / f"bad-{i}.mir"
            source.write_text(text + "\n", encoding="ascii")
            check(*cephyr, "--emit-mir", source, "-o", output, expected=diagnostic)
            if output.exists():
                raise AssertionError(f"unexpected output: {output}")


if __name__ == "__main__":
    main()
