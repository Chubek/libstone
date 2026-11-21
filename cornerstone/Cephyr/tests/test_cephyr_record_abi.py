"""Exercise the RV64 LP64 record call boundary with independently compiled C."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "record_abi"
TRIPLE = "riscv64-linux-gnu"
NEGATIVE = (
    ("overaligned-definition", "struct R { _Alignas(16) long x; }; "
     "struct R f(struct R r) { return r; }", "records aligned above 8 bytes"),
    ("overaligned-direct", "struct R { _Alignas(16) long x; }; void g(struct R); "
     "void f(void) { struct R r = {1}; g(r); }", "records aligned above 8 bytes"),
    ("overaligned-indirect", "struct R { _Alignas(16) long x; }; "
     "void f(struct R (*g)(void)) { g(); }", "records aligned above 8 bytes"),
    ("overaligned-address", "struct R { _Alignas(16) long x; }; void g(struct R); "
     "typedef void F(struct R); F *f(void) { return g; }", "records aligned above 8 bytes"),
    ("wide-slots", "struct R { long x, y; }; struct R f("
     + ", ".join(f"struct R p{i}" for i in range(64)) + ") { return p0; }",
     "exceeds the native argument slot limit"),
    ("fp-member", "struct R { long x; double y; }; void f(struct R r) {}",
     "requires integer or pointer members"),
    ("incomplete", "struct R; struct R f(void) { for (;;) {} }",
     "requires a complete structure or union"),
    ("flexible", "struct R { int n; char bytes[]; }; void f(struct R r) {}",
     "without volatile members or flexible arrays"),
    ("variadic", "struct R { int n; }; int g(struct R, ...); "
     "void f(void) { struct R r = {0}; g(r, 1); }", "requires a fixed prototype"),
    ("variadic-trailing", "struct R { int n; }; int g(int, ...); "
     "void f(void) { struct R r = {0}; g(1, r); }", "variadic argument"),
    ("unspecified", "struct R { int n; }; struct R f() { struct R r = {0}; return r; }",
     "requires a fixed prototype"),
    ("volatile-member", "struct R { volatile int x; }; void f(struct R r) {}",
     "without volatile members or flexible arrays"),
    ("wrong-argument", "struct R { int x; }; struct S { int x; }; void g(struct R); "
     "void f(void) { struct S s = {1}; g(s); }", "error"),
    ("wrong-result", "struct R { int x; }; struct S { int x; }; "
     "struct R f(void) { struct S s = {1}; return s; }", "error"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--lld", default="ld.lld")
    parser.add_argument("--qemu", default=shutil.which("qemu-riscv64") or
                        shutil.which("qemu-riscv64-static"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-record-abi-") as directory:
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

        base = (str(args.cephyr.resolve()), "-std=c11")
        cephyr = (*base, f"--target={TRIPLE}")
        clang = (args.clang, f"--target={TRIPLE}", "-march=rv64im", "-mabi=lp64")
        native, harness, entry, executable = (work / name for name in
                                               ("native.o", "harness.o", "start.o", "test"))
        start = work / "start.s"
        start.write_text(
            '.text\n.globl _start\n_start:\ncall test_main\nli a7,93\necall\n'
            '.section .note.GNU-stack,"",@progbits\n', encoding="ascii")
        check(*clang, "-c", start, "-o", entry)
        check(*clang, "-std=c11", "-O2", "-ffreestanding", "-fno-pie", "-fno-stack-protector",
              "-c", FIXTURES / "harness.c", "-o", harness)

        def run_binary(mode):
            if platform.machine().lower() == "riscv64":
                check(executable)
            elif args.qemu:
                check(args.qemu, executable)
            else:
                print(f"{TRIPLE} {mode}: linked; execution skipped (no emulator)")

        for mode, options in (("plain", ()), ("qml", ("--qml-pass=canonicalize",)),
                              ("pic", ("-fPIC",))):
            check(*cephyr, *options, "-c", FIXTURES / "native.c", "-o", native)
            check(args.lld, "-m", "elf64lriscv", "-static", entry, native, harness, "-o", executable)
            run_binary(mode)

        check(*cephyr, "--stdlib=cephyr", FIXTURES / "division.c", "-o", executable)
        run_binary("bundled libc division")

        def reject(compiler, name, text, diagnostic):
            source = work / f"{name}.c"
            output = work / f"{name}.mir"
            source.write_text(text + "\n", encoding="ascii")
            check(*compiler, "--emit-mir", source, "-o", output, expected=diagnostic)
            if output.exists():
                raise AssertionError(f"{name}: unexpectedly emitted {output}")

        for name, text, diagnostic in NEGATIVE:
            reject(cephyr, name, text, diagnostic)
        for arch in ("x86_64", "aarch64"):
            other = (*base, f"--target={arch}-linux-gnu")
            reject(other, arch,
                   "struct R { int n; }; struct R f(struct R r) { return r; }",
                   "only supported on the bundled RV64IM LP64 backend")
            source = work / f"{arch}-division-header.c"
            source.write_text(
                "#include <stdlib.h>\n#include <inttypes.h>\n"
                "#if defined(__CEPHYR_HAS_NATIVE_DIVISION_API) || "
                "!defined(__CEPHYR_STRUCT_VALUE_API_UNAVAILABLE)\n"
                "#error aggregate division declarations must remain gated\n#endif\n",
                encoding="ascii")
            check(*other, "--stdlib=cephyr", "-fsyntax-only", source)


if __name__ == "__main__":
    main()
