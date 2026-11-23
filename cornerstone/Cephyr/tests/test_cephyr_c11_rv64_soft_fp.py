"""RV64IM LP64 soft-float arithmetic and fixed-prototype ABI against GCC."""

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
    ("float __addsf3(float, float); float f(float x) { return x + x; }",
     "reserved for the RV64 soft-float runtime"),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--cc", default=os.environ.get("CEPHYR_TEST_CC_RV64") or "riscv64-linux-gnu-gcc")
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
        cc = (args.cc, "-march=rv64im", "-mabi=lp64", "-mno-relax", "-fno-pic")
        asm, native, harness, exe = (work / name for name in
                                     ("native.s", "native.o", "harness.o", "test"))
        start = work / "start.s"
        start.write_text('.text\n.globl _start\n_start:\ncall test_main\nli a7,93\necall\n'
                         '.section .note.GNU-stack,"",@progbits\n', encoding="ascii")
        check(*cc, "-std=c11", "-O2", "-ffreestanding", "-fno-pie", "-fno-stack-protector",
              "-c", FIXTURES / "harness.c", "-o", harness)

        def run_binary():
            if args.qemu:
                check(args.qemu, "-cpu", "rv64i,m=true", exe)
            elif platform.machine().lower() == "riscv64":
                check(exe)
            else:
                print(f"{TRIPLE}: linked; execution skipped (no emulator)")

        # Test both default startup and an explicit freestanding entry, in
        # addition to the canonicalizer and PIC's helper relocations.
        for options, startup in (((), ()), (("--qml-pass=canonicalize",), ("-nostartfiles", start)),
                                 (("-fPIC",), ())):
            check(*cephyr, *options, "-S", FIXTURES / "native.c", "-o", asm)
            # Assembly must be accepted by a strict RV64IM assembler.
            check(*cc, "-c", asm, "-o", native)
            check(*cephyr, *options, "-c", FIXTURES / "native.c", "-o", native)
            # ELF64 little-endian e_flags: RVC and floating ABI bits must be clear.
            (flags,) = struct.unpack_from("<I", native.read_bytes(), 48)
            if flags & 7:
                raise AssertionError(f"unexpected RV64IM LP64 flags: {flags:#x}")
            check(*cephyr, "--stdlib=cephyr", *startup, native, harness, "-o", exe)
            run_binary()
        for i, (text, diagnostic) in enumerate(NEGATIVE):
            source, output = work / f"bad-{i}.c", work / f"bad-{i}.mir"
            source.write_text(text + "\n", encoding="ascii")
            check(*cephyr, "--emit-mir", source, "-o", output, expected=diagnostic)
            if output.exists():
                raise AssertionError(f"unexpected output: {output}")


if __name__ == "__main__":
    main()
