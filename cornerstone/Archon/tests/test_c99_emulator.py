"""Generate native emulators and exercise them against the Python runtime."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


ROOT = Path(__file__).resolve().parents[1]
TOY8 = ROOT / "examples" / "toy8.arch"
RISCV = ROOT / "stdspec" / "riscv"

EDGE_CASES = """
architecture EdgeCases {
    endian = big;
    address_width = 64;
    register_file Registers {
        count = 3;
        width = 64;
        register r[0..2];
        zero = 0;
    }
    register_class GPR { members = Registers.*; }

    format ShiftFormat: bits<16> {
        field opcode: bits<3> @ [15:13];
        field rd: bits<2> @ [12:11];
        field value: sint<4> @ [10:7];
        field amount: uint<7> @ [6:0];
    }
    instruction RIGHT {
        operands { out rd: GPR; in value: imm<sint<4>>; in amount: imm<uint<7>>; }
        encoding ShiftFormat { opcode = 1; rd = rd; value = value; amount = amount; }
        assembly "right {rd}, {value}, {amount}";
        semantics { rd := sext<64>(value) >> amount; }
    }
    instruction LEFT {
        operands { out rd: GPR; in value: imm<sint<4>>; in amount: imm<uint<7>>; }
        encoding ShiftFormat { opcode = 2; rd = rd; value = value; amount = amount; }
        assembly "left {rd}, {value}, {amount}";
        semantics { rd := zext<64>(value) << amount; }
    }

    format Triple: bits<16> {
        field opcode: bits<10> @ [15:6];
        field scratch: bits<2> @ [5:4];
        field lhs: bits<2> @ [3:2];
        field rhs: bits<2> @ [1:0];
    }
    instruction PAIR {
        operands { inout scratch: GPR; inout lhs: GPR; inout rhs: GPR; }
        encoding Triple { opcode = 0x300; scratch = scratch; lhs = lhs; rhs = rhs; }
        assembly "pair {scratch}, {lhs}, {rhs}";
        semantics {
            sequence {
                scratch := read(scratch) +% 1;
                parallel {
                    let saved = read(rhs);
                    lhs := saved +% 3;
                    rhs := read(lhs) +% 4;
                }
            }
        }
    }

    format Fixed: bits<16> { field opcode: bits<16> @ [15:0]; }
    instruction ENCODING_ONLY {
        encoding Fixed { opcode = 0x1234; }
        assembly "encoding_only";
    }
}
"""


def _octets(data):
    return ", ".join(f"0x{byte:02x}" for byte in data)


def _uint64s(values):
    return ", ".join(f"UINT64_C(0x{value % (1 << 64):x})" for value in values)


class C99Emulator(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.directory = Path(tmp.name)
        self.db_path = self.directory / "target.archdb"

    def cli(self, *args, ok=True):
        result = subprocess.run(
            [sys.executable, "-m", "archon", *map(str, args)],
            cwd=ROOT, capture_output=True, check=False,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b"Traceback", result.stderr)
        return result

    def compiler(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        return cc

    def native(self, cc, body):
        harness = self.directory / "test.c"
        harness.write_text(
            '#include "emu.h"\n#include "emu.h"\n#include <assert.h>\n#include <string.h>\n'
            f"int main(void) {{\n{body}\n    return 0;\n}}\n",
            encoding="ascii",
        )
        exe = self.directory / "test"
        subprocess.run(
            [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", "emu.c", "test.c", "-o", exe],
            cwd=self.directory, check=True,
        )
        subprocess.run([exe], cwd=self.directory, check=True)

    def generate(self, db, prefix):
        database.save(db, self.db_path)
        self.cli(
            "gen", self.db_path, "--emulator", "--prefix", prefix,
            "-o", self.directory / "emu.c", "--header", self.directory / "emu.h",
        )

    def test_cli_selects_emulator_without_changing_default_codec(self):
        self.cli("compile", TOY8, "-o", self.db_path)
        default = self.cli("gen", self.db_path).stdout
        self.assertIn(b"archon_encode(", default)
        self.assertNotIn(b"archon_emu_execute(", default)
        self.assertIn(b"archon_emu_execute(", self.cli("gen", self.db_path, "--emulator").stdout)
        generated = self.directory / "named.c"
        header = self.directory / "named.h"
        args = ("gen", self.db_path, "--emulator", "--prefix", "toy")
        self.cli(*args, "-o", generated, "--header", header)
        self.assertIn(b"toy_emu_execute(", generated.read_bytes())
        self.assertIn(b"TOY_EMU_NO_SEMANTICS", header.read_bytes())
        self.assertEqual(generated.read_bytes(), self.cli(*args).stdout)
        self.assertEqual(header.read_bytes(), self.cli(*args, "-o", generated, "--header", "-").stdout)
        self.assertIn(b"prefix", self.cli("gen", self.db_path, "--emulator", "--prefix", "9toy", ok=False).stderr)
        self.assertIn(b"requires --emulator", self.cli("gen", self.db_path, "--header", header, ok=False).stderr)

    def test_cli_rejects_database_as_output_even_via_links(self):
        self.cli("compile", TOY8, "-o", self.db_path)
        saved = self.db_path.read_bytes()
        hard_link, symbolic_link = self.directory / "hard.c", self.directory / "sym.c"
        hard_link.hardlink_to(self.db_path)
        symbolic_link.symlink_to(self.db_path)
        for mode in ((), ("--emulator",)):
            for path in (self.db_path, hard_link, symbolic_link):
                with self.subTest(mode=mode, path=path):
                    self.assertIn(b"output path", self.cli("gen", self.db_path, *mode, "-o", path, ok=False).stderr)
                    self.assertEqual(self.db_path.read_bytes(), saved)
        args = ("gen", self.db_path, "--emulator")
        out = self.directory / "emu.c"
        self.assertIn(b"header path", self.cli(*args, "-o", out, "--header", symbolic_link, ok=False).stderr)
        self.assertFalse(out.exists())
        self.assertIn(b"destinations", self.cli(*args, "-o", out, "--header", out, ok=False).stderr)
        self.assertFalse(out.exists())
        self.assertIn(b"destinations", self.cli(*args, "--header", "-", ok=False).stderr)
        out.write_bytes(b"preserve this")
        out_link = self.directory / "emu.h"
        out_link.hardlink_to(out)
        self.assertIn(b"destinations", self.cli(*args, "-o", out, "--header", out_link, ok=False).stderr)
        self.assertEqual(out.read_bytes(), b"preserve this")
        self.assertEqual(self.db_path.read_bytes(), saved)

    def test_toy8_program_from_cli(self):
        cc = self.compiler()
        binary = self.directory / "toy8.bin"
        self.cli("compile", TOY8, "-o", self.db_path)
        self.cli("asm-program", self.db_path, ROOT / "examples" / "toy8.s", "-o", binary)
        self.cli(
            "gen", self.db_path, "--emulator",
            "-o", self.directory / "emu.c", "--header", self.directory / "emu.h",
        )
        program = binary.read_bytes()
        initial = [71, 5, 9, 0, 0, 0, 0, 0]
        expected = Target(database.load(self.db_path)).execute_program(program, initial)
        self.native(cc, f"""
    const uint8_t code[] = {{{_octets(program)}}};
    const uint64_t expected[] = {{{_uint64s(expected)}}};
    const uint64_t swap_same[] = {{1, 1}};
    uint64_t regs[] = {{{_uint64s(initial)}}};
    size_t consumed = 123;
    assert(archon_emu_register_count() == 8);
    assert(archon_emu_register_width() == 8);
    assert(archon_emu_run(code, sizeof code, regs, 8, &consumed) == ARCHON_EMU_OK);
    assert(consumed == sizeof code);
    assert(memcmp(regs, expected, sizeof regs) == 0);
    assert(archon_emu_execute("SWAP", swap_same, 2, regs, 8) == ARCHON_EMU_OK);
    assert(memcmp(regs, expected, sizeof regs) == 0);
    regs[4] = 256;
    assert(archon_emu_step(code, 2, regs, 8) == ARCHON_EMU_INVALID_ARGUMENT);
    assert(regs[1] == expected[1] && regs[4] == 256);
""")

    def test_riscv_32_and_64_match_python_for_each_instruction(self):
        cc = self.compiler()
        for width in (32, 64):
            with self.subTest(width=width):
                db = compile_file(RISCV / f"rv{width}i.arch")
                target = Target(db)
                self.generate(db, "rv")
                initial = [91, (1 << width) - 1, 2, 1 << (width - 1), *([0] * 28)]
                state = initial
                expected_steps, named_operands = [], []
                lines = [
                    "add x4, x1, x2", "sub x5, x3, x1", "xor x6, x4, x5",
                    "or x7, x6, x3", "and x8, x7, x5", "addi x9, x0, -2048",
                    "xori x10, x9, -1", "ori x11, x10, 2047", "andi x12, x11, -1",
                    "addi x0, x12, 1",
                ]
                for line in lines:
                    name, operands = target.assemble(line)
                    instruction = next(inst for inst in db["instructions"] if inst["name"] == name)
                    values = [operands[op["name"]] for op in instruction["operands"]]
                    named_operands.append(f'        {{"{name}", {{{_uint64s(values)}}}}},')
                    state = target.execute(name, operands, state)
                    expected_steps.append(f"        {{{_uint64s(state)}}},")
                program = target.assemble_program("\n".join(lines))
                self.native(cc, f"""
    const uint8_t code[] = {{{_octets(program)}}};
    const uint64_t initial[] = {{{_uint64s(initial)}}};
    const struct {{ const char *name; uint64_t operands[3]; }} ops[] = {{
{chr(10).join(named_operands)}
    }};
    const uint64_t expected[][32] = {{
{chr(10).join(expected_steps)}
    }};
    uint64_t step_regs[32], execute_regs[32];
    const size_t count = sizeof ops / sizeof ops[0];
    size_t consumed = 0;
    assert(rv_emu_register_width() == {width});
    assert(rv_emu_register_count() == 32);
    memcpy(step_regs, initial, sizeof initial);
    memcpy(execute_regs, initial, sizeof initial);
    for (size_t i = 0; i < count; ++i) {{
        assert(rv_emu_step(code + i * 4, 4, step_regs, 32) == RV_EMU_OK);
        assert(rv_emu_execute(ops[i].name, ops[i].operands, 3, execute_regs, 32) == RV_EMU_OK);
        assert(memcmp(step_regs, expected[i], sizeof step_regs) == 0);
        assert(memcmp(execute_regs, expected[i], sizeof execute_regs) == 0);
    }}
    memcpy(step_regs, initial, sizeof initial);
    assert(rv_emu_run(code, sizeof code, step_regs, 32, &consumed) == RV_EMU_OK);
    assert(consumed == sizeof code && memcmp(step_regs, expected[count - 1], sizeof step_regs) == 0);
""")

    def test_shifts_and_instruction_failure_atomicity(self):
        cc = self.compiler()
        source = self.directory / "edge.arch"
        source.write_text(EDGE_CASES, encoding="ascii")
        db = compile_file(source)
        target = Target(db)
        self.generate(db, "edge")
        initial = [99, 10, 20]
        shifts = target.assemble_program("""
            right r1, -8, 1
            right r2, -8, 63
            right r2, -8, 64
            left r1, -1, 63
            left r2, -1, 64
            pair r1, r1, r2
            pair r2, r0, r0
        """)
        expected = target.execute_program(shifts, initial)
        conflict = target.assemble_program("pair r2, r1, r1")
        with self.assertRaisesRegex(ArchonError, "conflicting parallel writes"):
            target.execute_program(conflict, initial)
        good = target.assemble_program("right r1, -8, 1")
        after_good = target.execute_program(good, initial)
        no_semantics = target.assemble_program("encoding_only")
        # Shift encoding with invalid rd=3; it fits the field but is not a register.
        bad_reg = (int.from_bytes(good, "big") | (3 << 11)).to_bytes(2, "big")
        prefixes = [
            f"        {{{_uint64s(target.execute_program(shifts[:end], initial))}}},"
            for end in range(2, len(shifts) + 1, 2)
        ]
        self.native(cc, f"""
    const uint8_t shifts[] = {{{_octets(shifts)}}};
    const uint8_t conflict[] = {{{_octets(conflict)}}};
    const uint8_t encoding_only[] = {{{_octets(no_semantics)}}};
    const uint8_t unknown[] = {{0xff, 0xff}};
    const uint8_t bad_reg[] = {{{_octets(bad_reg)}}};
    const uint8_t with_conflict[] = {{{_octets(good + conflict)}}};
    const uint8_t truncated[] = {{{_octets(good + conflict[:1])}}};
    const uint64_t initial[] = {{{_uint64s(initial)}}};
    const uint64_t expected[] = {{{_uint64s(expected)}}};
    const uint64_t expected_steps[][3] = {{
{chr(10).join(prefixes)}
    }};
    const uint64_t after_good[] = {{{_uint64s(after_good)}}};
    const uint64_t unextended[] = {{1, 8, 1}};
    const uint64_t aliases[] = {{2, 1, 1}};
    uint64_t regs[3];
    size_t consumed = 123;
    memcpy(regs, initial, sizeof regs);
    assert(edge_emu_execute("RIGHT", unextended, 3, regs, 3) == EDGE_EMU_OPERAND_RANGE);
    assert(edge_emu_execute("RIGHT", unextended, 2, regs, 3) == EDGE_EMU_INVALID_ARGUMENT);
    assert(edge_emu_execute("RIGHT", NULL, 3, regs, 3) == EDGE_EMU_INVALID_ARGUMENT);
    assert(edge_emu_execute("PAIR", aliases, 3, regs, 3) == EDGE_EMU_CONFLICTING_WRITES);
    assert(edge_emu_execute("ENCODING_ONLY", NULL, 0, regs, 3) == EDGE_EMU_NO_SEMANTICS);
    assert(edge_emu_execute("BOGUS", NULL, 0, regs, 3) == EDGE_EMU_UNKNOWN_INSTRUCTION);
    assert(edge_emu_step(conflict, sizeof conflict, regs, 3) == EDGE_EMU_CONFLICTING_WRITES);
    assert(edge_emu_step(encoding_only, sizeof encoding_only, regs, 3) == EDGE_EMU_NO_SEMANTICS);
    assert(edge_emu_step(unknown, sizeof unknown, regs, 3) == EDGE_EMU_UNKNOWN_INSTRUCTION);
    assert(edge_emu_step(bad_reg, sizeof bad_reg, regs, 3) == EDGE_EMU_OPERAND_RANGE);
    assert(edge_emu_step(shifts, 1, regs, 3) == EDGE_EMU_TRUNCATED_INSTRUCTION);
    assert(edge_emu_step(shifts, 2, regs, 2) == EDGE_EMU_INVALID_ARGUMENT);
    assert(edge_emu_step(shifts, sizeof shifts, regs, 3) == EDGE_EMU_INVALID_ARGUMENT);
    assert(edge_emu_run(NULL, 0, regs, 3, &consumed) == EDGE_EMU_OK);
    assert(consumed == 0);
    assert(memcmp(regs, initial, sizeof regs) == 0);
    assert(edge_emu_run(with_conflict, sizeof with_conflict, regs, 3, &consumed) == EDGE_EMU_CONFLICTING_WRITES);
    assert(consumed == 2 && memcmp(regs, after_good, sizeof regs) == 0);
    memcpy(regs, initial, sizeof regs);
    assert(edge_emu_run(truncated, sizeof truncated, regs, 3, &consumed) == EDGE_EMU_TRUNCATED_INSTRUCTION);
    assert(consumed == 2 && memcmp(regs, after_good, sizeof regs) == 0);
    memcpy(regs, initial, sizeof regs);
    assert(edge_emu_run(shifts, sizeof shifts, regs, 3, &consumed) == EDGE_EMU_OK);
    assert(consumed == sizeof shifts && memcmp(regs, expected, sizeof regs) == 0);
    memcpy(regs, initial, sizeof regs);
    for (size_t offset = 0; offset < sizeof shifts; offset += 2) {{
        assert(edge_emu_step(shifts + offset, 2, regs, 3) == EDGE_EMU_OK);
        assert(memcmp(regs, expected_steps[offset / 2], sizeof regs) == 0);
    }}
""")


if __name__ == "__main__":
    unittest.main()
