"""Exercise the Python runtime directly against a hand-built ASIR database."""

from copy import deepcopy
import unittest

from archon.diagnostics import ArchonError
from archon.runtime import Target


def database(endian="big"):
    word = {"kind": "bits", "width": 8}
    return {
        "version": 1,
        "architecture": "Tiny",
        "address_width": 8,
        "endian": endian,
        "parameters": {},
        "features": [],
        "registers": {
            "name": "Registers", "prefix": "$", "count": 3, "width": 8,
            "zero": 0, "aliases": {"zero": 0, "a": 1, "b": 2},
        },
        "instructions": [
            {
                "id": "Tiny.BUMP",
                "name": "BUMP",
                "width": 16,
                "mask": 0xF000,
                "value": 0xA000,
                "operands": [
                    {"name": "rd", "direction": "out", "kind": "reg", "width": 2, "signed": False},
                    {"name": "base", "direction": "in", "kind": "reg", "width": 2, "signed": False},
                    {"name": "offset", "direction": "in", "kind": "imm", "width": 8, "signed": True},
                ],
                "fields": [
                    {"operand": "offset", "lsb": 0, "width": 8},
                    {"operand": "base", "lsb": 8, "width": 2},
                    {"operand": "rd", "lsb": 10, "width": 2},
                ],
                "assembly": "bump {rd}, {offset}({base})",
                "semantics": {
                    "word_width": 8,
                    "effects": {"reads": ["base"], "writes": ["rd"]},
                    "statements": [{
                        "op": "write", "name": "rd",
                        "value": {
                            "op": "+%", "type": word,
                            "left": {"op": "read", "type": word, "name": "base"},
                            "right": {
                                "op": "immediate", "type": {"kind": "sint", "width": 8},
                                "name": "offset",
                            },
                        },
                    }],
                },
            },
            {
                "id": "Tiny.IDLE", "name": "IDLE", "width": 16,
                "mask": 0xFFFF, "value": 0xB000, "operands": [], "fields": [],
                "assembly": "idle", "semantics": None,
            },
        ],
    }


class TargetTests(unittest.TestCase):
    def test_round_trip_and_endianness(self):
        operands = {"rd": 1, "base": 2, "offset": -1}
        for endian, encoding in (("big", b"\xa6\xff"), ("little", b"\xff\xa6")):
            with self.subTest(endian=endian):
                target = Target(database(endian))
                self.assertEqual(target.assemble(" \tbump\ta , -0x1 ( b ) "), ("BUMP", operands))
                self.assertEqual(target.encode("BUMP", operands), encoding)
                self.assertEqual(target.decode(encoding), ("BUMP", operands))
                self.assertEqual(target.disassemble(encoding), "bump $1, -1($2)")
                self.assertEqual(target.assemble(target.disassemble(encoding)), ("BUMP", operands))

    def test_signed_immediate_limits(self):
        target = Target(database())
        for number, byte in ((-128, 0x80), (127, 0x7F)):
            with self.subTest(number=number):
                inst = target.assemble(f"bump $1,{number}($2)")
                self.assertEqual(target.encode(*inst), bytes([0xA6, byte]))
                self.assertEqual(target.decode(bytes([0xA6, byte])), inst)
        for number in (-129, 128):
            with self.subTest(number=number), self.assertRaises(ArchonError):
                target.assemble(f"bump $1,{number}($2)")

    def test_execute_register_state(self):
        target = Target(database())
        initial = [45, 0, 255]
        cases = [
            ("bump a, 2(b)", [0, 1, 255]),
            ("bump b, 4(b)", [0, 0, 3]),
            ("bump a, -1(zero)", [0, 255, 255]),
            ("bump zero, 1(b)", [0, 0, 255]),
        ]
        for text, expected in cases:
            with self.subTest(text=text):
                name, operands = target.assemble(text)
                self.assertEqual(target.execute(name, operands, initial), expected)
                self.assertEqual(initial, [45, 0, 255])
        name, operands = target.assemble("bump a, 1(b)")
        for registers in ([0, 0], [0, 0, 256], [0, 0, True]):
            with self.subTest(registers=registers), self.assertRaises(ArchonError):
                target.execute(name, operands, registers)

    def test_instruction_without_semantics(self):
        target = Target(database())
        self.assertEqual(target.assemble("idle"), ("IDLE", {}))
        self.assertEqual(target.encode("IDLE", {}), b"\xb0\x00")
        self.assertEqual(target.disassemble(b"\xb0\x00"), "idle")
        with self.assertRaisesRegex(ArchonError, "no executable semantics"):
            target.execute("IDLE", {}, [0, 0, 0])

    def test_strict_assembly(self):
        target = Target(database())
        for text in (
            "bad", "BUMP a, 1(b)", "bumpa, 1(b)", "bump a 1(b)", "bump a, 1b", "bump a, 1 b",
            "bump a, 1(b), c", "bump a, 1(b) # comment", "bump a,\n1(b)", "bump a, 0x(b)",
            "bump a, 1(2)", "bump $3, 1(b)", "bump a, 1_0(b)",
        ):
            with self.subTest(text=text), self.assertRaises(ArchonError):
                target.assemble(text)

    def test_bad_encodings_and_operands(self):
        target = Target(database())
        for data in (b"", b"\xa6", b"\xa6\x00\x00", b"\xc0\x00", b"\xa7\xff", "a6ff"):
            with self.subTest(data=data), self.assertRaises(ArchonError):
                target.decode(data)
        values = {"rd": 1, "base": 2, "offset": 0}
        for name, operands in (
            ("UNKNOWN", values), ("BUMP", {}), ("BUMP", {**values, "extra": 0}),
            ("BUMP", {**values, "rd": 3}), ("BUMP", {**values, "rd": True}),
            ("BUMP", {**values, "offset": 128}), ("BUMP", {**values, "offset": "1"}),
        ):
            with self.subTest(name=name, operands=operands), self.assertRaises(ArchonError):
                target.encode(name, operands)

    def test_empty_target(self):
        target = Target({**database(), "instructions": []})
        with self.assertRaisesRegex(ArchonError, "no enabled instructions"):
            target.assemble("idle")
        with self.assertRaisesRegex(ArchonError, "no enabled instructions"):
            target.decode(b"\xb0\x00")

    def test_invalid_database_tables(self):
        for change, error in (
            ({"width": 4097}, "width"),
            ({"mask": 0x1F000}, "mask and value"),
            ({"value": 0xA001}, "mask and value"),
            ({"fields": []}, "field for every operand"),
            ({"mask": 0xE000}, "entire encoding"),
            ({"assembly": "bump {rd.attr}, {offset}({base})"}, "placeholder"),
            ({"assembly": "bump {rd}, {offset}({rd})"}, "repeats"),
            ({"assembly": "bump {rd}, {offset}"}, "every operand"),
        ):
            with self.subTest(change=change):
                db = database()
                db["instructions"][0].update(change)
                with self.assertRaisesRegex(ArchonError, error):
                    Target(db)
        db = database()
        db["instructions"][0]["fields"][2]["lsb"] = 11
        with self.assertRaisesRegex(ArchonError, "overlaps"):
            Target(db)
        db = database()
        db["instructions"][0]["fields"][2]["width"] = 1
        with self.assertRaisesRegex(ArchonError, "width differs from its operand"):
            Target(db)
        db = database()
        db["instructions"].append({**deepcopy(db["instructions"][0]), "name": "BUMP2"})
        with self.assertRaisesRegex(ArchonError, "overlapping encodings"):
            Target(db)
        db = database()
        db["registers"]["count"] = 65537
        with self.assertRaisesRegex(ArchonError, "register count"):
            Target(db)

    def test_invalid_semantic_payload(self):
        db = database()
        db["instructions"][1]["semantics"] = {"word_width": 8, "statements": [None]}
        target = Target(db)
        with self.assertRaisesRegex(ArchonError, "malformed executable semantics"):
            target.execute("IDLE", {}, [0, 0, 0])


if __name__ == "__main__":
    unittest.main()
