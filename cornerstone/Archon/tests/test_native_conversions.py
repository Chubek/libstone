"""Native integer conversion legality and RV64 execution."""

import itertools
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory

from test_irgen_native import EXAMPLE, RV64IM, RV_FLAGS, TMP_ROOT, run, unicorn_exit_status


OPS = ("trunc", "zext", "sext")
WORD_MASK = (1 << 64) - 1


def conversion(name, op, width, src_type="i64", dst_type="i64"):
    return (
        f"function {name} {dst_type} 1 2 1 0\n"
        f"types {src_type} {dst_type}\n"
        f"block 0 1\nparams\n{op} 1 0 {width}\nret 1\n"
    )


def module(functions):
    return (f"archon-mir 1\nmodule {len(functions)} 0 0\n"
            + "".join(functions) + "end\n").encode("ascii")


class NativeConversionsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        cmake = shutil.which("cmake")
        if not cc or not cmake:
            raise unittest.SkipTest("a host C compiler and CMake are required")
        tmp = tempfile.TemporaryDirectory(
            prefix="archon-conversions-", dir=TMP_ROOT if TMP_ROOT.is_dir() else None,
        )
        cls.addClassCleanup(tmp.cleanup)
        cls.work = Path(tmp.name)
        generated = write_generated_directory(
            generate_ir("gated", [compile_file(RV64IM)],
                        options={"profile": "native", "oracle": False}),
            cls.work / "generated",
        )
        build = cls.work / "build"
        run(cmake, "-S", generated, "-B", build, f"-DCMAKE_C_COMPILER={cc}",
            "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic", cwd=cls.work)
        run(cmake, "--build", build, "--target", "archon-backend", cwd=cls.work)
        cls.driver = build / "archon-backend"

    def compile(self, functions):
        return subprocess.run(
            [self.driver, "--input", "-"], input=module(functions),
            cwd=self.work, capture_output=True, timeout=10,
        )

    def test_widths_and_types(self):
        for op in OPS:
            for width in (1, 32, 63, 64):
                with self.subTest(op=op, width=width):
                    result = self.compile([conversion("convert", op, width)])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(b"convert:", result.stdout)
            for width in (0, 65, (1 << 32) + 1, WORD_MASK):
                with self.subTest(op=op, width=width):
                    result = self.compile([conversion("convert", op, width)])
                    self.assertEqual((result.returncode, result.stdout), (1, b""))
                    self.assertIn(b"width must be in 1..64", result.stderr)
            for src, dst in (("ptr", "i64"), ("i64", "ptr"),
                             ("func_ptr", "i64"), ("i64", "func_ptr")):
                with self.subTest(op=op, src=src, dst=dst):
                    result = self.compile([conversion("convert", op, 8, src, dst)])
                    self.assertEqual((result.returncode, result.stdout), (1, b""))
                    self.assertIn(b"integer conversion requires (i64) -> i64", result.stderr)

    def test_execute_rv64(self):
        clang, lld = shutil.which("clang"), shutil.which("ld.lld")
        if not clang or not lld:
            self.skipTest("Clang and ld.lld are required for RV64 assemble/link")
        if not re.search(r"^\s*riscv64\s+-", run(clang, "--print-targets", cwd=self.work), re.M):
            self.skipTest("Clang lacks RV64 support")

        functions, names, declarations, checks = [], [], [], []
        for i, (op, width) in enumerate(itertools.product(OPS, range(1, 65)), 1):
            name = f"convert_{op}_{width}"
            functions.append(conversion(name, op, width))
            names.append(name)
            declarations.append(f"extern unsigned long {name}(unsigned long);")
            sign_bit = 1 << (width - 1)
            low_mask = (1 << width) - 1
            high_mask = WORD_MASK ^ low_mask
            for data in sorted({0, sign_bit, high_mask, high_mask | sign_bit,
                                high_mask | (sign_bit - 1), WORD_MASK}):
                expected = data & low_mask
                if op == "sext" and expected & sign_bit:
                    expected |= high_mask
                checks.append(f"if ({name}(0x{data:x}UL) != 0x{expected:x}UL) return {i};")
        result = self.compile(functions)
        self.assertEqual(result.returncode, 0, result.stderr)
        (self.work / "conversions.s").write_bytes(result.stdout)
        (self.work / "check.c").write_text(
            "\n".join([*declarations, "int target_main(void) {", *checks, "return 0;", "}"]),
            encoding="ascii",
        )
        for source, obj in (("conversions.s", "conversions.o"), ("check.c", "check.o"),
                            (EXAMPLE / "start.S", "start.o")):
            run(clang, *RV_FLAGS, "-O2", "-ffreestanding", "-fno-pic", "-fno-pie",
                "-fno-stack-protector", "-c", source, "-o", obj, cwd=self.work)
        program = self.work / "check"
        run(lld, "-m", "elf64lriscv", "-static", "-e", "_start",
            "start.o", "conversions.o", "check.o", "-o", program, cwd=self.work)

        qemu = (os.environ.get("ARCHON_QEMU_RISCV64") or shutil.which("qemu-riscv64")
                or shutil.which("qemu-riscv64-static"))
        if qemu:
            result = subprocess.run(
                ["sh", "-c", 'ulimit -c 0; exec "$@"', "qemu", qemu, program],
                cwd=self.work, capture_output=True, timeout=10,
            )
            status = result.returncode
        else:
            try:
                import unicorn
                from unicorn import riscv_const
            except ImportError:
                self.skipTest("linked RV64 ELF; execution requires QEMU or Unicorn")
            if not unicorn.uc_arch_supported(unicorn.UC_ARCH_RISCV):
                self.skipTest("linked RV64 ELF; installed Unicorn lacks RISC-V support")
            status = unicorn_exit_status(program, unicorn, riscv_const)
        failed = names[status - 1] if isinstance(status, int) and 1 <= status <= len(names) else status
        self.assertEqual(status, 0, f"RV64 conversion check failed: {failed}")


if __name__ == "__main__":
    unittest.main()
