"""Execution conformance for opt-in passes using the runtime frontend."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory
from test_irgen_native import EXAMPLE, RV_FLAGS, unicorn_exit_status


ROOT = Path(__file__).resolve().parents[1]

# Dynamic stack and pointer features are intentionally avoided here: this
# checks the same scalar results with and without the new optional scheduling.
PROGRAM = """archon-mir 1
module 2 1 0
function followup i64 1 11 1 0
types i64 i64 i64 i64 i64 i64 i64 i64 i64 func_ptr i64
block 0 10
params
const 1 11
const 2 0xff
sext 3 2 8
const 4 5
mul_checked_s 5 0 4
sub 6 5 1
add 7 6 3
zext 8 7 8
func_addr 9 callback
call_indirect 10 9 signature 1 8
ret 10
function callback i64 1 2 1 0
types i64 i64
block 0 1
params
trunc 1 0 7
ret 1
import signature i64 1 i64
end
"""

TARGET = """extern long followup(long);
int target_main(void)
{
    return followup(3) == 3 && followup(-2) == 106 ? 0 : 1;
}
"""


class NativeFollowup(unittest.TestCase):
    def test_opt_in_modes_execute_same_native_program(self):
        paths = {tool: shutil.which(tool) for tool in ("cmake", "cc", "clang", "ld.lld")}
        if not all(paths.values()):
            self.skipTest("CMake, a host compiler, Clang and LLD are required")
        if b"riscv64" not in subprocess.check_output([paths["clang"], "--print-targets"], timeout=10):
            self.skipTest("Clang does not support RV64")
        try:
            import unicorn
            from unicorn import riscv_const
        except ImportError:
            unicorn = None
        tmp = Path("/tmp/opencode")
        with tempfile.TemporaryDirectory(dir=tmp if tmp.is_dir() else None) as raw:
            work = Path(raw)
            build = work / "build"
            generated = work / "generated"
            spec = ROOT / "stdspec" / "riscv" / "rv64im.arch"
            write_generated_directory(
                generate_ir("gated", [compile_file(spec)], options={"profile": "native", "oracle": False}),
                generated,
            )
            self.run_cmd(paths["cmake"], "-S", generated, "-B", build)
            self.run_cmd(paths["cmake"], "--build", build, "--target", "archon-backend")
            mir, target = work / "test.mir", work / "test.c"
            mir.write_text(PROGRAM, encoding="ascii")
            target.write_text(TARGET, encoding="ascii")
            start_obj, target_obj = work / "start.o", work / "test.o"
            self.run_cmd(paths["clang"], *RV_FLAGS, "-c", EXAMPLE / "start.S", "-o", start_obj)
            self.run_cmd(paths["clang"], *RV_FLAGS, "-ffreestanding", "-fno-pic", "-fno-pie",
                         "-c", target, "-o", target_obj)
            driver = build / "archon-backend"
            for i, flags in enumerate(((), ("--schedule-pure",))):
                with self.subTest(flags=flags):
                    asm, obj, result = (work / f"program-{i}.{ext}" for ext in ("s", "o", "elf"))
                    asm.write_bytes(self.run_cmd(driver, "--input", mir, *flags))
                    self.run_cmd(paths["clang"], *RV_FLAGS, "-c", asm, "-o", obj)
                    self.run_cmd(paths["ld.lld"], "-m", "elf64lriscv", "-static",
                                 "-e", "_start", start_obj, target_obj, obj, "-o", result)
                    if unicorn is not None and unicorn.uc_arch_supported(unicorn.UC_ARCH_RISCV):
                        self.assertEqual(unicorn_exit_status(result, unicorn, riscv_const), 0)
            records = self.run_cmd(driver, "--input", mir, "--schedule-pure", "--emit-physical")
            self.assertTrue(records.startswith(b"archon-physical 1"), records[:120])
            self.assertNotIn(b".option", records)
            self.assertIn(b"followup", records)

    def run_cmd(self, *args):
        result = subprocess.run(args, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        return result.stdout
