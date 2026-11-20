"""Check the generated native backend's source-order scheduling contract."""

import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory


ROOT = Path(__file__).resolve().parents[1]
RV64IM = ROOT / "stdspec" / "riscv" / "rv64im.arch"
PROGRAM = """\
archon-mir 1
module 1 1 0
function preserve_order void 2 5 1 0
types ptr i64 i64 i64 i64
block 0 5
params
sub 2 1 1
store 0 2 0
call - observe 0
load 3 0 8
xor 4 3 1
trap
import observe void 0
end
"""


class NativeSchedulerTests(unittest.TestCase):
    def test_generated_backend_keeps_arithmetic_memory_call_and_trap_in_order(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]) or not shutil.which("cmake"):
            self.skipTest("host C99 compiler and CMake required")

        with tempfile.TemporaryDirectory(prefix="archon-schedule-") as directory:
            root = Path(directory)
            generated = write_generated_directory(
                generate_ir(
                    "gated", [compile_file(RV64IM)],
                    options={"profile": "native", "abi": "LP64", "oracle": False},
                ),
                root / "generated",
            )
            build = root / "build"
            subprocess.run(
                [
                    "cmake", "-S", generated, "-B", build,
                    f"-DCMAKE_C_COMPILER={';'.join(cc)}",
                    "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic",
                ],
                check=True, cwd=root, timeout=60,
            )
            subprocess.run(
                ["cmake", "--build", build, "--target", "archon-backend"],
                check=True, cwd=root, timeout=60,
            )
            result = subprocess.run(
                [build / "archon-backend", "--input", "-"],
                input=PROGRAM, capture_output=True, text=True, cwd=root,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

        # Ignore spill and frame traffic (x2); inspect only the observable
        # memory locations and the lowering of the surrounding operations.
        operations = []
        for row in result.stdout.splitlines():
            line = row.strip()
            opcode = line.split(" ", 1)[0]
            if (opcode in {"sub", "xor", "ebreak"} or line == "call observe" or
                    re.fullmatch(r"sd x\d+, 0\(x(?!2\))\d+\)", line) or
                    re.fullmatch(r"ld x\d+, 8\(x(?!2\))\d+\)", line)):
                operations.append(opcode)
        self.assertEqual(operations, ["sub", "sd", "call", "ld", "xor", "ebreak"])


if __name__ == "__main__":
    unittest.main()
