"""Exercise bundled RV64IM LP64 stdarg against Clang without a target sysroot."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "rv64_stdarg"
TRIPLE = "riscv64-linux-gnu"
NEGATIVE = (
    ("void f(int x) { va_list a; va_start(a, x); }", "rightmost eligible named parameter"),
    ("void f(int x, int y, ...) { va_list a; va_start(a, x); }", "rightmost eligible named parameter"),
    ("void f(register int x, ...) { va_list a; va_start(a, x); }", "rightmost eligible named parameter"),
    ("void f(float x, ...) { va_list a; va_start(a, x); }", "rightmost eligible named parameter"),
    ("void f(int x, ...) { void *const a = 0; va_start(a, x); }", "writable RV64IM LP64 va_list"),
    ("void f(int x, ...) { va_list a; va_arg(a, float); }", "only promoted integer"),
    ("void f(int x, ...) { va_list a; va_arg(a, short); }", "only promoted integer"),
    ("struct S { int x; }; void f(va_list a) { va_arg(a, struct S); }", "only promoted integer"),
    ("struct S { int x; }; void f(struct S s, ...) {}", "requires a fixed prototype"),
    ("struct S { int x; }; void g(int, ...); void f(struct S *s) { g(1, *s); }",
     "variadic argument"),
    ("void f(void (*p)(int, ...)) { p(0, 1); }", "variadic indirect calls"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--qemu", default=shutil.which("qemu-riscv64") or
                        shutil.which("qemu-riscv64-static"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-rv64-stdarg-") as directory:
        work = Path(directory)
        env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        env.pop("CEPHYR_CONFIG", None)

        def check(*command, expected=None):
            result = subprocess.run(command, cwd=work, env=env, text=True, capture_output=True,
                                    timeout=300)
            if expected is None and result.returncode != 0:
                raise AssertionError((command, result.returncode, result.stdout, result.stderr))
            if expected is not None and (result.returncode <= 0 or expected not in result.stderr):
                raise AssertionError((command, expected, result.returncode, result.stderr))

        cephyr = (args.cephyr.resolve(), "-std=c11", f"--target={TRIPLE}")
        clang = (args.clang, f"--target={TRIPLE}", "-march=rv64im", "-mabi=lp64", "-mno-relax")
        check(*clang, "-std=c11", "-O2", "-ffreestanding", "-fno-pie",
              "-fno-stack-protector", "-Wall", "-Wextra", "-Werror", "-c",
              FIXTURES / "external.c", "-o", "external.o")
        start = work / "start.s"
        start.write_text('.text\n.globl _start\n_start:\ncall test_main\nli a7,93\necall\n'
                         '.section .note.GNU-stack,"",@progbits\n', encoding="ascii")
        for name, options in (("default", ()), ("qml", ("--qml-pass=canonicalize",)),
                              ("pic", ("-fPIC",))):
            mir = work / f"{name}.mir"
            check(*cephyr, *options, "--emit-mir", FIXTURES / "native.c", "-o", mir)
            if "rv64_va_start" not in mir.read_text(encoding="ascii").split():
                raise AssertionError(f"missing rv64_va_start in {mir}")
            asm, obj, exe = (work / f"{name}.{suffix}" for suffix in ("s", "o", "elf"))
            check(*cephyr, *options, "-S", FIXTURES / "native.c", "-o", asm)
            check(*clang, "-c", asm, "-o", obj)
            check(*cephyr, *options, "-c", FIXTURES / "native.c", "-o", obj)
            check(*cephyr, "--stdlib=cephyr", "-nostartfiles", start, obj, "external.o", "-o", exe)
            if args.qemu:
                check(args.qemu, "-cpu", "rv64i,m=true", exe)
            elif platform.machine().lower() == "riscv64":
                check(exe)
            else:
                print(f"{TRIPLE} ({name}): linked; execution skipped (no emulator)")

        for i, (text, diagnostic) in enumerate(NEGATIVE):
            source, output = work / f"reject-{i}.c", work / f"reject-{i}.mir"
            source.write_text("#include <stdarg.h>\n" + text + "\n", encoding="ascii")
            check(*cephyr, "--emit-mir", source, "-o", output, expected=diagnostic)
            if output.exists():
                raise AssertionError(f"unexpected output: {output}")


if __name__ == "__main__":
    main()
