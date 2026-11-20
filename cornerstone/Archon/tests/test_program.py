"""Whole-file assembly, byte layout, disassembly and linear execution."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from archon.diagnostics import ArchonError, Location
from archon.runtime import Target


ROOT = Path(__file__).resolve().parents[1]


def target(endian="big", *, signed=True, empty=False):
    return Target({
        "version": 1,
        "endian": endian,
        "registers": {
            "prefix": "$", "count": 3, "width": 8, "zero": 0,
            "aliases": {"zero": 0, "a": 1, "b": 2},
        },
        "instructions": [] if empty else [{
            "name": "BUMP", "width": 16, "mask": 0xF000, "value": 0xA000,
            "operands": [
                {"name": "dst", "direction": "out", "kind": "reg", "width": 2, "signed": False},
                {"name": "src", "direction": "in", "kind": "reg", "width": 2, "signed": False},
                {"name": "offset", "direction": "in", "kind": "imm", "width": 8, "signed": signed},
            ],
            "fields": [
                {"operand": "offset", "lsb": 0, "width": 8},
                {"operand": "src", "lsb": 8, "width": 2},
                {"operand": "dst", "lsb": 10, "width": 2},
            ],
            "assembly": "bump {dst}, #{offset}({src})",
            "semantics": None,
        }],
    })


class ProgramTests(unittest.TestCase):
    def test_forward_backward_and_eof_labels_with_two_byte_instructions(self):
        source = (
            "# comments and mixed line endings\r\n \t\r"
            "start: other: bump a, #end(b)# inline comment\n"
            "mid: bump a, #@start - 0x2(b)\r\n"
            "     bump a, #@end+0b11(b)\n"
            "end: # points past the last instruction"
        )
        for endian, encoded in (("big", "a626a6fca605"), ("little", "26a6fca605a6")):
            with self.subTest(endian=endian):
                machine = target(endian)
                data = machine.assemble_program(source, base_address=0x20)
                self.assertEqual(data.hex(), encoded)
                text = machine.disassemble_program(data, base_address=0x20)
                self.assertEqual(text, "bump $1, #38($2)\nbump $1, #-4($2)\nbump $1, #5($2)\n")
                self.assertEqual(machine.assemble_program(text), data)

    def test_labels_only_in_immediates_and_single_instruction_unchanged(self):
        machine = target()
        self.assertEqual(machine.assemble_program("a: bump a, #a(b)"), b"\xa6\x00")
        self.assertEqual(machine.assemble_program("a: bump a, #@a(b)"), b"\xa6\x00")
        self.assertEqual(machine.assemble("bump a, #1(b)"), ("BUMP", {"dst": 1, "src": 2, "offset": 1}))
        for text in ("a: bump a, #a(b)", "bump a, #a(b)", "bump a, #1(b) # comment", "bump a, #@a(b)"):
            with self.subTest(text=text), self.assertRaises(ArchonError):
                machine.assemble(text)
        for instruction in ("bump @a, #1(b)", "bump a, #1(@a)", "bump a, #a+-2(b)", "bump a, #a + b(b)"):
            with self.subTest(instruction=instruction):
                with self.assertRaisesRegex(ArchonError, "does not match"):
                    machine.assemble_program(f"a: {instruction}")

    def test_diagnostics_include_source_coordinates(self):
        machine = target()
        for source, line, column, message in (
            ("\nfirst:\n first: bump a, #0(b)", 3, 2, "duplicate label 'first' .*line 2"),
            ("# comment\r\n\tentry: bump a, #@typo(b)", 2, 9, "unknown label 'typo'"),
            ("Foo:\n  bump a, #foo(b)", 2, 3, "unknown label 'foo'"),
            ("a:\n  bump $3, #a(b)", 2, 3, "not a valid register index"),
            ("a:\n  other", 2, 3, "does not match"),
        ):
            with self.subTest(source=source):
                with self.assertRaisesRegex(ArchonError, message) as caught:
                    machine.assemble_program(source, filename="sample.s")
                self.assertEqual(caught.exception.location, Location("sample.s", line, column))
        machine.assemble_program("local:")
        with self.assertRaisesRegex(ArchonError, "unknown label 'local'"):
            machine.assemble_program("bump a, #local(b)")

    def test_fixups_use_existing_immediate_bounds(self):
        for signed, low, high in ((True, -128, 127), (False, 0, 255)):
            machine = target(signed=signed)
            for value in (low, high):
                with self.subTest(signed=signed, value=value):
                    sign = "-" if value < 0 else "+"
                    self.assertEqual(
                        machine.assemble_program(f"same: bump a, #@same {sign} {abs(value)}(b)",
                                                 base_address=0x1020),
                        bytes([0xA6, value & 255]),
                    )
            for source, base in (
                (f"same: bump a, #@same - {abs(low) + 1}(b)", 0),
                ("same: bump a, #same(b)", high + 1),
            ):
                with self.subTest(signed=signed, source=source):
                    with self.assertRaisesRegex(ArchonError, r"<assembly>:1:.*does not fit"):
                        machine.assemble_program(source, base_address=base)

    def test_binary_diagnostics_and_empty_programs(self):
        machine = target()
        with self.assertRaisesRegex(ArchonError, r"0x22.*truncated instruction.*got 1"):
            machine.disassemble_program(b"\xa6\x00\xa6", base_address=0x20)
        with self.assertRaisesRegex(ArchonError, r"0x22.*unknown instruction encoding"):
            machine.disassemble_program(b"\xa6\x00\x00\x00", base_address=0x20)
        with self.assertRaisesRegex(ArchonError, r"0x20.*no executable semantics"):
            machine.execute_program(b"\xa6\x00", [0, 0, 0], base_address=0x20)
        with self.assertRaisesRegex(ArchonError, "encoded program must be bytes"):
            machine.disassemble_program("a600")
        with self.assertRaisesRegex(ArchonError, "base address must be a nonnegative integer"):
            machine.assemble_program("", base_address=True)
        with self.assertRaisesRegex(ArchonError, "base address must be a nonnegative integer"):
            machine.disassemble_program(b"", base_address=-1)
        for machine in (machine, target(empty=True)):
            self.assertEqual(machine.assemble_program("# empty\nend:\n"), b"")
            self.assertEqual(machine.disassemble_program(b""), "")
            initial = [0, 7, 3]
            final = machine.execute_program(b"", initial)
            self.assertEqual(final, initial)
            self.assertIsNot(final, initial)
        with self.assertRaisesRegex(ArchonError, r"<assembly>:2:.*no enabled instructions"):
            target(empty=True).assemble_program("\nbump a, #1(b)")


class ProgramCLI(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.db = self.directory / "riscv.archdb"
        self.cli("compile", ROOT / "stdspec/riscv/rv32i.arch", "-o", self.db)

    def cli(self, *args, stdin=None, ok=True):
        result = subprocess.run(
            [sys.executable, "-m", "archon", *map(str, args)], cwd=ROOT,
            input=stdin, capture_output=True, check=False,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b"Traceback", result.stderr)
        return result

    def test_file_assembly_disassembly_streams_and_linear_execution(self):
        assembly = b"start: addi x1, x0, after\naddi x2, x1, @start\nafter:\n"
        canonical = b"addi x1, x0, 24\naddi x2, x1, -4\n"
        data = bytes.fromhex("930080011381c0ff")
        source = self.directory / "example.s"
        binary = self.directory / "example.bin"
        output = self.directory / "canonical.s"
        source.write_bytes(assembly)
        self.cli("asm-program", self.db, source, "--base", "0x10", "-o", binary)
        self.assertEqual(binary.read_bytes(), data)
        self.assertEqual(self.cli("asm-program", self.db, "-", "--base", "16", stdin=assembly).stdout, data)
        self.cli("dis-program", self.db, binary, "-o", output)
        self.assertEqual(output.read_bytes(), canonical)
        self.assertEqual(self.cli("dis-program", self.db, "-", stdin=data).stdout, canonical)
        registers = json.loads(self.cli("run-program", self.db, source, "--base", "16", "--reg", "x1=42").stdout)
        self.assertEqual(registers["x1"], 24)
        self.assertEqual(registers["x2"], 20)

    def test_failures_produce_no_partial_stdout(self):
        result = self.cli("asm-program", self.db, "-", stdin=b"\naddi x1,x0,missing\n", ok=False)
        self.assertIn(b"<stdin>:2:1: unknown label 'missing'", result.stderr)
        self.assertEqual(result.stdout, b"")
        result = self.cli("dis-program", self.db, "-", "--base", "0x40", stdin=b"\x93\x00", ok=False)
        self.assertIn(b"0x40: truncated instruction", result.stderr)
        self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
