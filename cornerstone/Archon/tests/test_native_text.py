"""Runtime MachineIR text, generated driver, and the optional ELF bridge."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "stdspec" / "riscv" / "rv64im.arch"
PROGRAM = b"""archon-mir 1
module 1 1 1
function apply i64 2 6 3 1
types ptr i64 ptr i64 i64 i64
local 8 8
block 0 3
params
frame_addr 2 0
load 3 0 0
store 2 3 0
cbr 1 1 1 3 2 1 1
block 1 1
params 4
call - consume 1 2
ret 4
block 1 0
params 5
ret 5
import consume void 1 ptr
global datum 1 init 0x8000000000000000
end
"""

INDIRECT = b"""archon-mir 1
module 2 1 0
function invoke i64 1 4 1 0
types i64 func_ptr func_ptr i64
block 0 3
params
func_addr 1 target
copy 2 1
call_indirect 3 2 target 1 0
ret 3
function target i64 1 1 1 0
types i64
block 0 0
params
ret 0
import incompatible i64 1 ptr
end
"""

PIC_ADDRESSES = b"""archon-mir 1
module 1 1 1
function addresses i64 0 3 1 0
types ptr func_ptr i64
block 0 3
params
global_addr 0 ext_data
func_addr 1 ext_function
ptr_to_int 2 0
ret 2
import ext_function void 0
global ext_data 0 zero
end
"""


class RuntimeTextTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmake = shutil.which("cmake")
        if not cmake or not shutil.which("cc"):
            raise unittest.SkipTest("CMake and a host C compiler are required")
        temp = tempfile.TemporaryDirectory(prefix="archon-text-")
        cls.addClassCleanup(temp.cleanup)
        cls.work = Path(temp.name)
        generated = write_generated_directory(
            generate_ir("gated", [compile_file(SPEC)], options={"profile": "native", "oracle": False}),
            cls.work / "generated",
        )
        build = cls.work / "build"
        for args in (("-S", generated, "-B", build, "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic-errors"),
                     ("--build", build, "--target", "archon-backend")):
            subprocess.run([cmake, *args], check=True, capture_output=True, timeout=60)
        cls.driver = build / "archon-backend"

    def cli(self, *args, data=None):
        return subprocess.run(
            [self.driver, *args], input=data, capture_output=True, cwd=self.work, timeout=10,
        )

    def test_compiles_deterministically_from_file_and_stdin(self):
        inline = self.cli("--input", "-", data=PROGRAM)
        self.assertEqual(inline.returncode, 0, inline.stderr)
        self.assertIn(b"apply:", inline.stdout)
        self.assertIn(b"call consume", inline.stdout)
        self.assertIn(b"0x8000000000000000", inline.stdout)
        path = self.work / "program with spaces.mir"
        path.write_bytes(PROGRAM)
        self.assertEqual(self.cli("--input", path).stdout, inline.stdout)
        verify = self.cli("--verify-input", path)
        self.assertEqual((verify.returncode, verify.stdout), (0, b""))
        caps = self.cli("--list-capabilities")
        self.assertIn(b"RISCV64IM/LP64", caps.stdout)

    def test_rejects_malformed_data_without_publishing(self):
        for source, diagnostic in (
            (b"#include <anything>", b"archon-mir"),
            (PROGRAM.replace(b"mir 1", b"mir 2"), b"unsupported schema"),
            (PROGRAM.replace(b"consume", b"bad;\n.string"), b"invalid symbol"),
            (PROGRAM.replace(b"apply", b"ap\x00ply"), b"NUL"),
            (PROGRAM.replace(b"cbr 1 1 1 3", b"cbr 1 1 1 4"), b"dominate"),
            (PROGRAM.replace(b"0x8000000000000000", b"0x10000000000000000"), b"out of range"),
            (PROGRAM[:-4], b"end"),
            (PROGRAM + b"extra", b"trailing input"),
            (b" " * (4 * 1024 * 1024 + 1), b"input byte limit"),
        ):
            with self.subTest(diagnostic=diagnostic):
                result = self.cli("--input", "-", data=source)
                self.assertEqual((result.returncode, result.stdout), (1, b""))
                self.assertIn(diagnostic, result.stderr)

    def test_lp64_rejects_float_in_every_output_mode(self):
        floating = b"""archon-mir 1
module 1 0 0
function calc f64 1 2 1 0
types f64 f64
block 0 1
params
fneg 1 0
ret 1
end
"""
        floating_import = b"""archon-mir 1
module 0 1 0
import fn i64 1 f32
end
"""
        for data in (floating, floating_import):
            for flag in ((), ("--emit-code",), ("--emit-physical",), ("--pic",)):
                with self.subTest(data=data, flag=flag):
                    res = self.cli("--input", "-", *flag, data=data)
                    self.assertEqual((res.returncode, res.stdout), (1, b""))
                    self.assertIn(b"RV64 LP64 is soft-float", res.stderr)

    def test_indirect_target_types_and_known_signatures(self):
        compiled = self.cli("--input", "-", data=INDIRECT)
        self.assertEqual(compiled.returncode, 0, compiled.stderr)
        self.assertIn(b"lla x7, target", compiled.stdout)
        self.assertIn(b"jalr x1, 0(x7)", compiled.stdout)
        for text in (
            INDIRECT.replace(b"func_addr 1 target", b"func_addr 1 incompatible"),
            INDIRECT.replace(b"func_ptr", b"ptr"),
        ):
            bad = self.cli("--input", "-", data=text)
            self.assertEqual((bad.returncode, bad.stdout), (1, b""))
        bad = self.cli("--input", "-", data=INDIRECT.replace(
            b"func_addr 1 target", b"func_addr 1 incompatible"))
        self.assertIn(b"known indirect target has a different signature", bad.stderr)

    def test_pic_flag_materializes_external_addresses_and_links_shared_object(self):
        default = self.cli("--input", "-", data=PIC_ADDRESSES)
        pic = self.cli("--input", "-", "--pic", data=PIC_ADDRESSES)
        self.assertEqual((default.returncode, pic.returncode), (0, 0))
        self.assertNotIn(b"\t.option pic\n", default.stdout)
        self.assertIn(b"\tlla x7, ext_data\n", default.stdout)
        self.assertIn(b"\tlla x7, ext_function\n", default.stdout)
        self.assertIn(b"\t.option pic\n", pic.stdout)
        self.assertIn(b"\tla x7, ext_data\n", pic.stdout)
        self.assertIn(b"\tla x7, ext_function\n", pic.stdout)
        self.assertNotIn(b"\tlla ", pic.stdout)
        for output in ("--emit-physical", "--emit-code"):
            with self.subTest(output=output):
                rejected = self.cli("--input", "-", "--pic", output, data=PIC_ADDRESSES)
                self.assertEqual((rejected.returncode, rejected.stdout), (2, b""))
                self.assertIn(b"PIC requires assembly output", rejected.stderr)

        clang, lld = shutil.which("clang"), shutil.which("ld.lld")
        if not clang or not lld:
            self.skipTest("Clang and LLD are required for the PIC shared-link check")
        targets = subprocess.run(
            [clang, "--print-targets"], check=True, capture_output=True, timeout=10,
        )
        if b"riscv64" not in targets.stdout:
            self.skipTest("Clang lacks RV64 support")
        source, definitions = self.work / "pic.s", self.work / "pic-defs.s"
        obj, defs, shared = self.work / "pic.o", self.work / "pic-defs.o", self.work / "pic.so"
        source.write_bytes(pic.stdout)
        definitions.write_text(
            ".option norvc\n.text\n.globl ext_function\n"
            ".type ext_function,@function\next_function:\n\tret\n"
            ".data\n.p2align 3\n.globl ext_data\next_data:\n\t.quad 0\n",
            encoding="ascii",
        )
        target = ("--target=riscv64-linux-gnu", "-march=rv64im", "-mabi=lp64")
        for asm, output in ((source, obj), (definitions, defs)):
            subprocess.run(
                [clang, *target, "-c", asm, "-o", output],
                check=True, capture_output=True, timeout=30,
            )
        subprocess.run(
            [lld, "-m", "elf64lriscv", "-shared", "--no-relax", obj, defs, "-o", shared],
            check=True, capture_output=True, timeout=30,
        )
        image = shared.read_bytes()
        self.assertEqual(image[:7], b"\x7fELF\x02\x01\x01")
        self.assertEqual(int.from_bytes(image[16:20], "little"), (243 << 16) | 3)

    def test_elf_bridge_atomic_publication(self):
        clang = shutil.which("clang")
        if not clang:
            self.skipTest("Clang is required for the RV64 object bridge")
        targets = subprocess.run([clang, "--print-targets"], check=True, capture_output=True, timeout=10)
        if b"riscv64" not in targets.stdout:
            self.skipTest("Clang lacks RV64 support")
        out = self.work / "object with spaces.o"
        argv = [sys.executable, "-m", "archon", "native-object", "-", "--backend", self.driver,
                "--assembler", clang, "-o", out]
        result = subprocess.run(argv, input=PROGRAM, cwd=ROOT, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = out.read_bytes()
        self.assertEqual(actual[:7], b"\x7fELF\x02\x01\x01")
        self.assertEqual(int.from_bytes(actual[18:20], "little"), 243)
        result = subprocess.run(argv, input=PROGRAM[:-4], cwd=ROOT, capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(out.read_bytes(), actual)
