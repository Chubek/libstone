"""Typed ArchSem compilation and concrete interpretation."""

import json
import unittest

from archon.diagnostics import ArchonError, Location
from archon.semantics import compile_semantics, execute_semantics


def reg(name, direction):
    return {"name": name, "direction": direction, "kind": "reg", "width": 3, "signed": False}


def imm(name, signed, direction="in"):
    return {"name": name, "direction": direction, "kind": "imm", "width": 4, "signed": signed}


class Semantics(unittest.TestCase):
    operands = [
        reg("rd", "out"), reg("rs1", "in"), reg("rs2", "in"), reg("rw", "inout"), reg("other", "inout"),
        imm("offset", True), imm("count", False), imm("bad_output", True, "out"),
    ]
    location = Location("toy.arch", 17, 20)

    def compile(self, source, width=8):
        return compile_semantics(
            source, self.location, self.operands, width, {"Word": {"kind": "bits", "width": width}},
            parameters={"XLEN": width},
        )

    def execute(self, source, operands, registers, width=8, zero=None):
        return execute_semantics(self.compile(source, width), operands, registers, width, zero)

    def test_json_round_trip_and_effects(self):
        program = self.compile(
            """
            # Derive register effects even when a register is read twice.
            let a: Word = read(rs1);
            let b = a ^ read(rs2) ^ read(rs1);
            write(rd, b | read(rw));
            write(rw, read(rs2) & b);
            """
        )
        program = json.loads(json.dumps(program))
        self.assertEqual(program["effects"], {"reads": ["rs1", "rs2", "rw"], "writes": ["rd", "rw"]})
        state = [0, 0xFF, 0x13, 0x04, 0]
        values = {"rd": 4, "rs1": 1, "rs2": 2, "rw": 3}
        self.assertEqual(execute_semantics(program, values, state, 8, None), [0, 0xFF, 0x13, 0x13, 0x17])
        self.assertEqual(state, [0, 0xFF, 0x13, 0x04, 0])

    def test_wraparound_at_the_architectural_width(self):
        values = {"rd": 1, "rs1": 1, "rs2": 2}
        for width in (8, 32, 64):
            with self.subTest(width=width):
                state = [0, (1 << width) - 1, 3]
                source = "write(rd, read(rs1) +% read(rs2));"
                self.assertEqual(self.execute(source, values, state, width)[1], 2)
                source = "write(rd, (read(rs2) -% 0x4) *% 0b10);"
                self.assertEqual(self.execute(source, values, state, width)[1], (1 << width) - 2)

    def test_signed_inputs_and_explicit_width_changes(self):
        source = """
            let displacement: sint<XLEN> = sext<XLEN>(offset);
            write(rd, read(rs1) +% displacement);
        """
        self.assertEqual(self.compile(source)["effects"], {"reads": ["rs1"], "writes": ["rd"]})
        for offset in (-2, 0xE):
            with self.subTest(offset=offset):
                values = {"rd": 1, "rs1": 2, "offset": offset}
                self.assertEqual(self.execute(source, values, [0, 0, 1])[1], 0xFF)
        source = "write(rd, zext<8>(trunc<4>(read(rs1))));"
        self.assertEqual(self.execute(source, {"rd": 1, "rs1": 2}, [0, 0, 0xBE])[1], 0xE)
        source = "write(rd, zext<8>(offset));"
        self.assertEqual(self.execute(source, {"rd": 1, "offset": -2}, [0, 0])[1], 0xE)

    def test_shifts_are_bounded_and_use_the_left_operands_signedness(self):
        state = [0, 0, 0x80]
        values = {"rd": 1, "rs1": 2, "count": 1, "offset": -8}
        self.assertEqual(self.execute("write(rd, read(rs1) >> count);", values, state)[1], 0x40)
        self.assertEqual(self.execute("write(rd, sext<8>(offset) >> 2);", values, state)[1], 0xFE)
        self.assertEqual(self.execute("write(rd, sext<8>(offset) >> 255);", values, state)[1], 0xFF)
        self.assertEqual(self.execute("write(rd, read(rs1) << 255);", values, state)[1], 0)

    def test_operator_precedence_and_complement(self):
        source = "write(rd, ~(0xF & 1 +% 2 *% 3));"
        self.assertEqual(self.execute(source, {"rd": 0}, [0]), [0xF8])

    def test_updates_are_sequential_and_zero_is_hardwired(self):
        source = """
            write(rw, read(rw) +% 1);
            write(rd, read(rw));
        """
        values = {"rw": 1, "rd": 2}
        state = [99, 42, 0]
        self.assertEqual(self.execute(source, values, state), [99, 43, 43])
        self.assertEqual(self.execute(source, {"rw": 0, "rd": 2}, state, zero=0), [0, 42, 0])
        self.assertEqual(self.execute(source, values, state, zero=2), [99, 43, 0])
        self.assertEqual(state, [99, 42, 0])

    def test_parallel_swap_and_ordered_sequence_observation(self):
        source = """
            let increment = 1;
            sequence {
                rw := read(rw) +% increment;
                parallel {
                    let prev = read(other);
                    write(other, read(rw));
                    rw := prev +% read(other);
                }
                sequence { rd := read(rw) +% read(other); }
            }
        """
        program = self.compile(source)
        seq = program["statements"][1]
        parallel = seq["statements"][1]
        self.assertEqual(seq["op"], "sequence")
        self.assertEqual(parallel["op"], "parallel")
        self.assertEqual([s["op"] for s in parallel["statements"]], ["let", "write", "write"])
        self.assertEqual(program["effects"], {"reads": ["rw", "other"], "writes": ["rw", "other", "rd"]})
        state = [0, 3, 5, 0]
        values = {"rw": 1, "other": 2, "rd": 3}
        self.assertEqual(execute_semantics(program, values, state, 8, None), [0, 10, 4, 14])
        self.assertEqual(state, [0, 3, 5, 0])
        swap = "parallel { rw := read(other); other := read(rw); }"
        self.assertEqual(self.execute(swap, values, state), [0, 5, 3, 0])

    def test_parallel_physical_alias_policy(self):
        state = [99, 22, 0]
        aliases = {"rw": 1, "other": 1}
        equal = "parallel { rw := 0xff +% 2; other := 1; }"
        self.assertEqual(self.execute(equal, aliases, state), [99, 1, 0])
        for writes in ("rw := 1; other := 2;", "other := 2; rw := 1;", "rw := 1; rw := 2;"):
            with self.subTest(writes=writes):
                with self.assertRaisesRegex(ArchonError, "conflicting parallel writes to register 1"):
                    self.execute("parallel { " + writes + " }", aliases, state)
        self.assertEqual(state, [99, 22, 0])
        self.assertEqual(self.execute("sequence { rw := 1; other := 2; }", aliases, state), [99, 2, 0])
        source = "parallel { rw := 2; other := 3; rd := read(rw) +% read(other); }"
        self.assertEqual(self.execute(source, {"rw": 0, "other": 0, "rd": 2}, state, zero=0), [0, 22, 0])

    def test_block_lexical_scope(self):
        source = """
            sequence { let temp = 12; rw := temp; }
            parallel { let temp = 7; rw := temp; }
            let temp = 2;
            sequence { rd := read(rw) +% temp; }
        """
        self.assertEqual(self.execute(source, {"rw": 0, "rd": 1}, [0, 0]), [7, 9])
        for bad_source, error in (
            ("sequence { let temp = 0; } rd := temp;", "unbound name"),
            ("parallel { let temp = 0; } rd := temp;", "unbound name"),
            ("let temp = 0; sequence { let temp = 1; }", "duplicate binding"),
            ("sequence { let temp = 0; parallel { let temp = 1; } }", "duplicate binding"),
            ("sequence { let rw = 0; }", "duplicate binding"),
            ("parallel { rd := later; let later = 0; }", "unbound name"),
            ("parallel { sequence {} }", "may only contain let and write"),
            ("parallel { parallel {} }", "may only contain let and write"),
            ("sequence { rd := 0;", "expected '}'"),
            ("parallel {};", "expected a semantic statement"),
            ("parallel { rs1 := 1; }", "cannot write in operand"),
        ):
            with self.subTest(source=bad_source), self.assertRaisesRegex(ArchonError, error):
                self.compile(bad_source)
        with self.assertRaises(ArchonError) as caught:
            self.compile("sequence {\n    parallel {\n        rs1 := 1;\n    }\n}")
        self.assertEqual(caught.exception.location, Location("toy.arch", 19, 9))

    def test_extract_and_concat(self):
        source = """
            let lower: bits<4> = extract(read(rs1), 3, 0);
            let upper: bits<4> = extract(read(rs1), 7, 4);
            rd := concat(lower, upper);
        """
        program = self.compile(source)
        upper = program["statements"][1]["value"]
        self.assertEqual((upper["op"], upper["high"], upper["low"]), ("extract", 7, 4))
        self.assertEqual(program["statements"][2]["value"]["op"], "concat")
        self.assertEqual(self.execute(source, {"rd": 0, "rs1": 1}, [0, 0xCA]), [0xAC, 0xCA])
        self.assertEqual(self.execute("rd := concat(offset, count);",
                                      {"rd": 0, "offset": -2, "count": 5}, [0]), [0xE5])
        self.assertEqual(self.execute("rd := extract(read(rs1), 7, 0);", {"rd": 0, "rs1": 1}, [0, 0xCA]),
                         [0xCA, 0xCA])
        for source, error in (
            ("let x = extract(read(rs1), 8, 0);", "extract requires"),
            ("let x = extract(read(rs1), 1, 2);", "extract requires"),
            ("let x = extract(read(rs1), 7, count);", "invalid integer literal"),
            ("let x = concat(zext<4096>(offset), count);", "invalid semantic type"),
            ("rd := concat(read(rs1), count);", "write expects a 8-bit value"),
        ):
            with self.subTest(source=source), self.assertRaisesRegex(ArchonError, error):
                self.compile(source)

    def test_compile_rejects_invalid_forms_at_statement_location(self):
        cases = {
            "write(rd, read(rd));": "cannot read out operand",
            "write(rs1, 1);": "cannot write in operand",
            "write(offset, 1);": "requires a register operand",
            "write(rd, read(count));": "requires a register operand",
            "write(rd, bad_output);": "cannot read out operand",
            "write(rd, rs1);": "use read",
            "write(rd, unknown);": "unbound name",
            "write(rd, offset);": "write expects a 8-bit value",
            "write(rd, read(rs1) +% offset);": "incompatible types",
            "let tmp = offset & count;": "incompatible types",
            "write(rd, read(rs1) >> offset);": "shift count",
            "let tmp: bits<4> = offset;": "expects bits<4>, got sint<4>",
            "let tmp: Unbound = read(rs1);": "unknown type",
            "let rs1 = 0;": "duplicate binding",
            "let tmp = zext<UNBOUND>(count);": "unknown width parameter",
            "let tmp = sext<2>(offset);": "cannot narrow",
            "let tmp = trunc<8>(offset);": "cannot widen",
            "let tmp = zext<0>(count);": "invalid semantic type",
            "let tmp = zext<1000000000>(count);": "invalid semantic type",
            "write(rd, 0x100);": "literal does not fit",
            "write(rd, 0xBogus);": "invalid integer literal",
            "write(rd, load(rs1));": "unsupported ArchSem operation",
            "write(rd, 1 + 0);": r"found '\+'",
        }
        for source, error in cases.items():
            with self.subTest(source=source):
                with self.assertRaisesRegex(ArchonError, error) as caught:
                    self.compile("let ok = 0;\r\n  /* comment */\r    " + source)
                self.assertEqual(caught.exception.location, Location("toy.arch", 19, 5))

    def test_same_line_diagnostic_uses_origin_column(self):
        with self.assertRaises(ArchonError) as caught:
            self.compile("  write(rd, unknown);")
        self.assertEqual(caught.exception.location, Location("toy.arch", 17, 22))

    def test_expression_depth_is_bounded_for_serialization(self):
        with self.assertRaisesRegex(ArchonError, "nested too deeply"):
            self.compile("write(rd, " + " +% ".join(["0"] * 200) + ");")
        with self.assertRaisesRegex(ArchonError, "nested too deeply"):
            self.compile("rd := " + "(" * 128 + "1" + ")" * 128 + ";")

    def test_block_and_node_limits(self):
        source = "sequence {" * 64 + "rd := 1;" + "}" * 64
        self.assertEqual(self.execute(source, {"rd": 0}, [0]), [1])
        with self.assertRaisesRegex(ArchonError, "blocks are nested too deeply"):
            self.compile("sequence {" + source + "}")
        with self.assertRaisesRegex(ArchonError, "exceeds 65536 nodes"):
            self.compile("sequence {}" * 65537)

    def test_invalid_direct_execution_wire_forms(self):
        program = self.compile("parallel { rw := extract(read(other), 7, 0); }")
        program["statements"][0]["statements"][0]["value"]["high"] = 8
        with self.assertRaisesRegex(ArchonError, "invalid semantic extract"):
            execute_semantics(program, {"rw": 0, "other": 1}, [0, 0], 8, None)
        program["statements"][0]["statements"] = [None]
        with self.assertRaisesRegex(ArchonError, "malformed executable semantics"):
            execute_semantics(program, {}, [0, 0], 8, None)

    def test_runtime_reports_invalid_register_references(self):
        program = self.compile("write(rd, read(rs1));")
        for operands, error in (
            ({"rd": 1, "rs1": -1}, "out of range"),
            ({"rd": 1, "rs1": 2}, "out of range"),
            ({"rd": 1, "rs1": True}, "must be an integer"),
            ({"rd": 1}, "missing semantic operand"),
        ):
            with self.subTest(operands=operands):
                with self.assertRaisesRegex(ArchonError, error):
                    execute_semantics(program, operands, [0, 0], 8, None)


if __name__ == "__main__":
    unittest.main()
