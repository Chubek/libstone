"""SemIR wire form validation via the ArchDB loader and serializer."""

import json
import unittest

from archon.database import dumps, loads
from archon.diagnostics import ArchonError, Location
from archon.semantics import compile_semantics, execute_semantics


def database(source):
    operands = [
        {"name": name, "direction": "inout", "kind": "reg", "width": 2, "signed": False}
        for name in ("a", "b")
    ]
    program = compile_semantics(source, Location("wire.arch", 1, 1), operands, 8, {})
    return {
        "version": 1, "architecture": "Wire", "address_width": 8, "endian": "little",
        "parameters": {}, "features": [],
        "registers": {"name": "Regs", "prefix": "r", "count": 4, "width": 8,
                      "zero": 0, "aliases": {}},
        "instructions": [{
            "id": "Wire.Test", "name": "Test", "width": 8, "mask": 0xF0, "value": 0x10,
            "operands": operands, "assembly": "test {a}, {b}",
            "fields": [{"operand": "a", "lsb": 0, "width": 2}, {"operand": "b", "lsb": 2, "width": 2}],
            "semantics": program,
        }],
    }


class SemanticsDatabase(unittest.TestCase):
    def assert_invalid(self, db, message):
        for check in (lambda data: loads(json.dumps(data)), dumps):
            with self.subTest(check=check), self.assertRaisesRegex(ArchonError, message):
                check(db)

    def test_new_and_legacy_v1_round_trips(self):
        for source, expected in (
            ("let tmp = read(b); write(a, tmp); write(b, read(a));", [0, 0xBA, 0xBA]),
            (
                "sequence { parallel { "
                "a := concat(extract(read(b), 3, 0), extract(read(b), 7, 4)); "
                "b := read(a); } }",
                [0, 0xAB, 0x11],
            ),
        ):
            with self.subTest(source=source):
                db = database(source)
                self.assertEqual(loads(dumps(db)), db)
                program = loads(dumps(db))["instructions"][0]["semantics"]
                self.assertEqual(execute_semantics(program, {"a": 1, "b": 2}, [0, 0x11, 0xBA], 8, 0), expected)

        old = database("write(a, read(b));")
        for operand in old["instructions"][0]["operands"]:
            del operand["direction"]
        self.assertEqual(loads(dumps(old)), old)

    def test_malformed_block_shape_and_nesting(self):
        for children, message in (
            ({}, "statements must be a JSON array"),
            ([None], r"statements\[0\] must be a JSON object"),
            ([{"op": "sequence", "statements": []}], "a parallel block may only contain let and write"),
            ([{"op": "parallel", "statements": []}], "a parallel block may only contain let and write"),
            ([{"op": "bad"}], "unknown statement operation"),
        ):
            with self.subTest(children=children):
                db = database("parallel {}")
                db["instructions"][0]["semantics"]["statements"][0]["statements"] = children
                self.assert_invalid(db, message)
        db = database("sequence {}")
        del db["instructions"][0]["semantics"]["statements"][0]["statements"]
        self.assert_invalid(db, "missing .*statements")

    def test_escaped_or_shadowed_local_is_invalid(self):
        db = database("sequence { let x = 1; a := x; }")
        statements = db["instructions"][0]["semantics"]["statements"]
        statements.append(statements[0]["statements"].pop())
        self.assert_invalid(db, "unknown local 'x'")

        db = database("let x = 1; sequence { let y = 2; }")
        db["instructions"][0]["semantics"]["statements"][1]["statements"][0]["name"] = "x"
        self.assert_invalid(db, "duplicate binding 'x'")

    def test_malformed_extract_and_concat(self):
        for mutation, message in (
            ({"high": 8}, "invalid extract bounds"),
            ({"low": -1}, "invalid extract bounds"),
            ({"low": True}, "low must be a JSON integer"),
            ({"type": {"kind": "uint", "width": 4}}, "invalid result type for extract"),
            ({"type": {"kind": "bits", "width": 5}}, "invalid result type for extract"),
        ):
            with self.subTest(mutation=mutation):
                db = database("a := concat(extract(read(b), 3, 0), extract(read(b), 7, 4));")
                value = db["instructions"][0]["semantics"]["statements"][0]["value"]
                value["left"].update(mutation)
                self.assert_invalid(db, message)
        db = database("a := concat(extract(read(b), 3, 0), extract(read(b), 7, 4));")
        value = db["instructions"][0]["semantics"]["statements"][0]["value"]
        value["type"]["width"] = 7
        self.assert_invalid(db, "invalid result type for concat")
        del value["right"]
        self.assert_invalid(db, "missing .*right")

    def test_wrong_nested_effects_or_operand_directions(self):
        db = database("sequence { parallel { a := read(b); } }")
        instruction = db["instructions"][0]
        effects = instruction["semantics"]["effects"]
        effects["reads"] = []
        self.assert_invalid(db, "effects do not match")
        effects["reads"] = ["b", "b"]
        self.assert_invalid(db, "invalid or duplicate effect")
        effects["reads"] = ["b"]
        instruction["operands"][0]["direction"] = "in"
        self.assert_invalid(db, "invalid operand direction for 'a'")

    def test_import_guards_block_and_expression_depth(self):
        db = database("sequence {}")
        program = db["instructions"][0]["semantics"]
        for _ in range(64):
            program["statements"] = [{"op": "sequence", "statements": program["statements"]}]
        self.assert_invalid(db, "blocks are nested too deeply")

        db = database("a := 1;")
        statement = db["instructions"][0]["semantics"]["statements"][0]
        for _ in range(128):
            statement["value"] = {"op": "~", "type": {"kind": "bits", "width": 8},
                                  "value": statement["value"]}
        self.assert_invalid(db, "expression is nested too deeply")


if __name__ == "__main__":
    unittest.main()
