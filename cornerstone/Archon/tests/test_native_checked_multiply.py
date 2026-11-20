"""Differential execution of signed checked multiplication on native RV64IM."""

from pathlib import Path
import random
import re
import shutil
import struct
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory

from test_irgen_native import RV64IM, RV_FLAGS, TMP_ROOT, run


# Three independent values live across the multiply, occupying the allocator's
# register homes. Stores also distinguish success from a trap before side effects.
MIR = """archon-mir 1
module 1 0 0
function mul_checked i64 3 7 1 0
types i64 i64 ptr i64 i64 i64 i64
block 0 7
params
const 3 11
const 4 22
const 5 33
mul_checked_s 6 0 1
store 2 3 0
store 2 4 8
store 2 5 16
ret 6
end
"""

MIN_I64 = -(1 << 63)
MAX_I64 = (1 << 63) - 1
MASK_I64 = (1 << 64) - 1


def operand_pairs():
    edges = (
        MIN_I64, MIN_I64 + 1, MIN_I64 // 2 - 1, MIN_I64 // 2,
        -(1 << 32), -3037000500, -3037000499, -2, -1, 0, 1, 2,
        3037000499, 3037000500, 1 << 32, MAX_I64 // 2, MAX_I64 // 2 + 1,
        MAX_I64 - 1, MAX_I64,
    )
    for a in edges:
        for b in edges:
            yield a, b

    rng = random.Random(0xA4C0)
    for _ in range(256):
        # Vary bit widths to exercise many non-overflowing products too.
        width_a, width_b = rng.randint(1, 64), rng.randint(1, 64)
        a = rng.randrange(-(1 << (width_a - 1)), 1 << (width_a - 1))
        b = rng.randrange(-(1 << (width_b - 1)), 1 << (width_b - 1))
        yield a, b
        yield b, a


class NativeCheckedMultiplyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc") or shutil.which("clang")
        cmake = shutil.which("cmake")
        if not cc or not cmake:
            raise unittest.SkipTest("a host C compiler and CMake are required")
        TMP_ROOT.mkdir(parents=True, exist_ok=True)
        tmp = tempfile.TemporaryDirectory(prefix="archon-checked-mul-", dir=TMP_ROOT)
        cls.addClassCleanup(tmp.cleanup)
        cls.work = Path(tmp.name)
        generated = write_generated_directory(
            generate_ir("gated", [compile_file(RV64IM)],
                        options={"profile": "native", "abi": "LP64", "oracle": False}),
            cls.work / "generated",
        )
        build = cls.work / "build"
        run(cmake, "-S", generated, "-B", build, f"-DCMAKE_C_COMPILER={cc}",
            "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic", cwd=cls.work)
        run(cmake, "--build", build, "--target", "archon-backend", cwd=cls.work)
        source = cls.work / "multiply.mir"
        source.write_text(MIR, encoding="ascii")
        cls.asm = cls.work / "multiply.s"
        cls.asm.write_text(run(build / "archon-backend", "--input", source, cwd=cls.work),
                           encoding="ascii")

    def test_emits_multiply_and_trap(self):
        asm = self.asm.read_text(encoding="ascii")
        self.assertRegex(asm, r"(?m)^\s*mul\s")
        self.assertRegex(asm, r"(?m)^\s*ebreak$")

    def test_rv64im_differential(self):
        clang, lld = shutil.which("clang"), shutil.which("ld.lld")
        if not clang or not lld:
            self.skipTest("Clang and ld.lld are required to link RV64IM")
        if not re.search(r"^\s*riscv64\s+-", run(clang, "--print-targets", cwd=self.work), re.M):
            self.skipTest("Clang lacks an RV64 target")
        obj, binary = self.work / "multiply.o", self.work / "multiply.bin"
        entry, ret = 0x10000, 0x10ffc
        run(clang, *RV_FLAGS, "-c", self.asm, "-o", obj, cwd=self.work)
        # This single function starts .text and contains all its branch targets.
        run(lld, "-m", "elf64lriscv", "--oformat=binary", "-Ttext", hex(entry),
            "-e", "mul_checked", obj, "-o", binary, cwd=self.work)

        try:
            import unicorn
            from unicorn import riscv_const as regs
        except ImportError:
            self.skipTest("linked RV64IM; differential execution requires Unicorn")
        if not unicorn.uc_arch_supported(unicorn.UC_ARCH_RISCV):
            self.skipTest("installed Unicorn lacks RISC-V support")

        uc = unicorn.Uc(unicorn.UC_ARCH_RISCV, unicorn.UC_MODE_RISCV64)
        data = 0x20000
        uc.mem_map(entry, 4096)
        uc.mem_map(data, 4096)
        uc.mem_write(entry, binary.read_bytes())
        exceptions = []

        def interrupt(machine, exception, _):
            exceptions.append(exception)
            machine.emu_stop()

        uc.hook_add(unicorn.UC_HOOK_INTR, interrupt)
        untouched = bytes(24)
        live_values = struct.pack("<QQQ", 11, 22, 33)
        for a, b in operand_pairs():
            with self.subTest(a=a, b=b):
                exceptions.clear()
                uc.mem_write(data, untouched)
                uc.reg_write(regs.UC_RISCV_REG_SP, data + 4096)
                uc.reg_write(regs.UC_RISCV_REG_RA, ret)
                uc.reg_write(regs.UC_RISCV_REG_A0, a & MASK_I64)
                uc.reg_write(regs.UC_RISCV_REG_A1, b & MASK_I64)
                uc.reg_write(regs.UC_RISCV_REG_A2, data)
                try:
                    uc.emu_start(entry, ret, timeout=1_000_000, count=200)
                except unicorn.UcError as exc:
                    pc = uc.reg_read(regs.UC_RISCV_REG_PC)
                    if (exc.errno != unicorn.UC_ERR_INSN_INVALID
                            or uc.mem_read(pc, 4) != bytes.fromhex("73 00 10 00")):
                        raise
                    exceptions.append(3)  # Unicorn can report ebreak as invalid.

                exact = a * b
                if MIN_I64 <= exact <= MAX_I64:
                    self.assertEqual(exceptions, [])
                    self.assertEqual(uc.reg_read(regs.UC_RISCV_REG_PC), ret)
                    self.assertEqual(uc.reg_read(regs.UC_RISCV_REG_A0), exact & MASK_I64)
                    self.assertEqual(uc.mem_read(data, len(live_values)), live_values)
                else:
                    self.assertEqual(exceptions, [3])  # RISC-V breakpoint exception
                    self.assertEqual(uc.mem_read(data, len(untouched)), untouched)
