"""Compile and execute C11 static binary32/binary64 constant initializers."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import tempfile

from test_cephyr_floating import EMULATIONS, START, run


FIXTURE = Path(__file__).resolve().parent / "fixtures" / "c11_static_fp.c"
HEADERS = FIXTURE.parents[2] / "stdlib" / "include"
NEGATIVE = {
    "call": ("int g(void); static int x = g() + 1.f;", "arithmetic constant expression"),
    "object": ("extern int y; static float x = y + 1.f;", "arithmetic constant expression"),
    "shift": ("static int x = 1 ? 1 : (2.f << 1);", "arithmetic constant expression"),
    "division": ("static double x = 1.0 / (1.0 - 1.0);", "division by zero"),
    "float-overflow": ("static float x = 0x1.fffffep127f * 2.f;", "out of range"),
    "float-narrow": ("static float x = 0x1p100 + 0x1p128;", "out of range"),
    "signed-overflow": ("static int x = (int)(2147483647.0 + 1.0);", "out of range"),
    "unsigned-overflow": ("static unsigned x = -2.f + 1.f;", "out of range"),
    "narrow-integer": ("static signed char x = 127.5 + 0.5;", "out of range"),
    "case-ice": ("int f(int n) { switch (n) { case (int)(1.0+2.0): return 1; } return 0; }",
                 "constant expression"),
    "assert-ice": ('_Static_assert((int)(1.0 + 2.0) == 3, "not an ICE");',
                   "integer constant expression"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--lld", default="ld.lld")
    parser.add_argument("--qemu-aarch64", default=shutil.which("qemu-aarch64") or
                        shutil.which("qemu-aarch64-static"))
    args = parser.parse_args()
    host = platform.machine().lower()
    cephyr = (str(args.cephyr.resolve()), "-std=c11")
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-static-fp-") as directory:
        work = Path(directory)
        env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        env.pop("CEPHYR_CONFIG", None)

        def check(*command, expected=None):
            return run(*command, work=work, env=env, expected=expected)

        for arch in START:
            triple = f"{arch}-linux-gnu"
            native, entry, exe = (work / f"{arch}.{suffix}" for suffix in ("o", "start.o", "elf"))
            check(*cephyr, f"--target={triple}", "-I", HEADERS, "-c", FIXTURE, "-o", native)
            start = work / "start.s"
            start.write_text("\n".join((".text", ".globl _start", "_start:", *START[arch],
                                       '.section .note.GNU-stack,"",%progbits', "")), encoding="ascii")
            check(args.clang, f"--target={triple}", "-c", start, "-o", entry)
            check(args.lld, "-m", EMULATIONS[arch], "-static", entry, native, "-o", exe)
            if host in (("x86_64", "amd64") if arch == "x86_64" else ("aarch64", "arm64")):
                check(exe)
            elif arch == "aarch64" and args.qemu_aarch64:
                check(args.qemu_aarch64, exe)
            else:
                print(f"{triple}: linked; execution skipped (no emulator)")

        for name, (text, diagnostic) in NEGATIVE.items():
            source, output = work / f"{name}.c", work / f"{name}.s"
            source.write_text(text + "\n", encoding="ascii")
            check(*cephyr, "--target=x86_64-linux-gnu", "-S", source, "-o", output,
                  expected=diagnostic)
            if output.exists():
                raise AssertionError(f"{name}: unexpectedly published {output}")


if __name__ == "__main__":
    main()
