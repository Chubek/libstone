"""Check the generated native Cephyr MIR and its acsem interpreter."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest


CEPHYR = Path(__file__).resolve().parents[1]
ROOT = CEPHYR.parents[1]
ACSEM = ROOT / "scripts/acsem/acsem.pl"
SCHEMA = CEPHYR / "MIR.acsem"
GENERATE = CEPHYR / "cmake/generate_mir_model.py"
NATIVE = ROOT / "cornerstone/Archon/archon/irgen/native"


class MIRModelTests(unittest.TestCase):
    def test_native_projection_and_wire_coverage(self):
        with tempfile.TemporaryDirectory(prefix="cephyr-mir-model-") as tmp:
            out = Path(tmp)
            model = out / "MIR.json"
            header = out / "CephyrMIRModel.hpp"
            subprocess.run(["perl", ACSEM, "--emit", "json", SCHEMA, "-o", model], check=True)
            subprocess.run([sys.executable, GENERATE, model, header], check=True)

            # Test opcode coverage and fixed SSA operand counts against the
            # native parser and legalizer, not a second hand-maintained list.
            text = (NATIVE / "MIRText.c").read_text()
            wire_ops = dict(re.findall(r'\{"(\w+)", (ARCHON_MIR_\w+)\}', text))
            legalizer = (NATIVE / "Legalizer.c").read_text()
            shape = legalizer.split("unsigned an_shape(", 1)[1].split("static", 1)[0]
            shapes = {}
            for cases, flags in re.findall(r"((?:\s*case [^:]+:[^\n]*\n)+)\s*return ([^;]+);", shape):
                for case in re.findall(r"case (ARCHON_MIR_\w+):", cases):
                    shapes[case] = (flags.count("HAS_A") + flags.count("HAS_B"), "HAS_DST" in flags)
            self.assertTrue(wire_ops and all(op in shapes for op in wire_ops.values()))
            rows = {
                name: (int(arity), result)
                for name, result, arity in re.findall(
                    r'OpcodeInfo\{Opcode::\w+, "(\w+)", ResultKind::(\w+), (\d+),',
                    header.read_text(),
                )
            }
            self.assertEqual(set(rows), wire_ops.keys() | {"ret", "br", "cbr", "trap"})
            for name, opcode in wire_ops.items():
                arity, result = rows[name]
                self.assertEqual(shapes[opcode], (arity, result != "none"), name)

            exe = out / "mir_model_test"
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", "-I", out, "-I", CEPHYR / "include",
                 CEPHYR / "tests/mir_model.cpp", "-o", exe],
                check=True,
            )
            subprocess.run([exe], check=True)

    def test_generated_acsem_cpp(self):
        with tempfile.TemporaryDirectory(prefix="cephyr-mir-as-") as tmp:
            cpp = Path(tmp) / "acsem.cpp"
            exe = Path(tmp) / "cephyr-mir-acsem"
            subprocess.run(["perl", ACSEM, "--emit", "cpp", SCHEMA, "-o", cpp], check=True)
            subprocess.run(
                [os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
                 "-pedantic", cpp, "-o", exe],
                check=True,
            )
            for term, status, result in (
                ("DescribeOpcode(call_indirect())", 0, "completed token:optional 1 token:call token:call 0"),
                ("DescribeOpcode(rem_u())", 0, "completed token:required 2 token:none token:pure 0"),
                ("DescribeOpcode(stack_free())", 0, "completed token:none 1 token:none token:ordered 0"),
                ("DescribeOpcode(invalid_opcode())", 1, "failed"),
                ("CheckInput(Link(NodeIndex(4),effect()))", 0, "completed"),
                ("CheckInput(Link(NodeIndex(-1),data()))", 1, "failed"),
                ("CheckNode(N(operation_node(),BlockIndex(1),const(),i64(),ValueIndex(2),"
                 "Range(0,1),Word16x4(65535,65535,65535,65535),none))", 0, "completed"),
                ("CheckNode(N(operation_node(),BlockIndex(1),const(),i64(),ValueIndex(2),"
                 "Range(0,1),Word16x4(65536,0,0,0),none))", 1, "failed"),
            ):
                with self.subTest(term=term):
                    run = subprocess.run([exe, "--term", term], capture_output=True, text=True)
                    self.assertEqual(run.returncode, status, run.stderr)
                    self.assertEqual(run.stdout.strip(), result)


if __name__ == "__main__":
    unittest.main()
