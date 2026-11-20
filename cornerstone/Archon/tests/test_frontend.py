"""Tests for the Archon frontend and its source locations."""

from textwrap import dedent
import unittest

from archon.diagnostics import ArchonError, Location
from archon.frontend import parse


class FrontendTests(unittest.TestCase):
    filename = "test.arch"

    def parse(self, source):
        return parse(dedent(source).lstrip("\n"), self.filename)

    def test_architecture_declarations(self):
        root = self.parse("""
            architecture CPU {
                import isa.integer;
                endian = little;
                address_width = XLEN;
                description = "A \\"small\\" CPU";
                version = "1.0";
                parameter XLEN: nat in {0x20, 0b1000000} = 64;
                feature I = enabled;
                feature M;
                feature D requires F, I;
                type Word = bits<XLEN>;
                type Alias = Word;
            }
            """)
        self.assertEqual(root["kind"], "architecture")
        self.assertEqual(root["name"], "CPU")
        self.assertEqual(root["loc"], Location(self.filename, 1, 1))
        declarations = root["declarations"]
        self.assertEqual(declarations[0], {
            "kind": "import", "path": "isa.integer", "name": "isa.integer",
            "loc": Location(self.filename, 2, 5),
        })
        self.assertEqual(
            [(node["kind"], node["name"], node["value"]) for node in declarations[1:5]],
            [("setting", "endian", "little"), ("setting", "address_width", "XLEN"),
             ("setting", "description", 'A "small" CPU'), ("setting", "version", "1.0")],
        )
        self.assertEqual(declarations[5], {
            "kind": "parameter", "name": "XLEN", "values": [32, 64], "default": 64,
            "loc": Location(self.filename, 7, 5),
        })
        self.assertEqual(
            [(node["name"], node["enabled"], node["requires"]) for node in declarations[6:9]],
            [("I", True, []), ("M", False, []), ("D", False, ["F", "I"])],
        )
        self.assertEqual(declarations[9]["spec"], {"kind": "bits", "arg": "XLEN"})
        self.assertEqual(declarations[10]["spec"], {"kind": "named", "arg": "Word"})

    def test_isa_declarations(self):
        root = self.parse("""
            module isa.integer {
                register_file Registers {
                    alias zero = x[0];
                    count = 32;
                    width = XLEN;
                    register x[0..31];
                    zero = 0;
                }
                register_class GPR { members = Registers.*; }
                format R: bits<16> {
                    field opcode: uint<4> @ [3:0];
                    field k: sint<12> @ [15:4];
                }
                instruction ADD {
                    requires feature(I);
                    requires feature(M);
                    operands {
                        inout rd: GPR;
                        in k: imm<sint<12>>;
                    }
                    encoding R { opcode=0xA; rd=encode_register(rd); k=k; }
                    assembly { syntax "add {rd}, {k}"; }
                    semantics { write(rd,read(rd) +% sext<XLEN>(k)); }
                }
                instruction NOP {
                    encoding R { opcode = +0b0; k = -0x2; }
                    assembly "nop";
                }
            }
            """)
        self.assertEqual((root["kind"], root["name"]), ("module", "isa.integer"))
        regs, reg_class, format_, add, nop = root["declarations"]
        self.assertEqual(regs, {
            "kind": "register_file", "name": "Registers", "loc": Location(self.filename, 2, 5),
            "count": 32, "width": "XLEN", "prefix": "x", "start": 0, "end": 31,
            "zero": 0, "aliases": {"zero": 0},
        })
        self.assertEqual(reg_class["file"], "Registers")
        self.assertEqual(format_["width"], 16)
        self.assertEqual(format_["fields"][0], {
            "name": "opcode", "spec": {"kind": "uint", "arg": 4}, "msb": 3, "lsb": 0,
            "loc": Location(self.filename, 11, 9),
        })
        self.assertEqual(add["requires"], ["I", "M"])
        self.assertEqual(add["operands"], [
            {"name": "rd", "direction": "inout", "type": "GPR", "loc": Location(self.filename, 18, 13)},
            {"name": "k", "direction": "in", "type": {"kind": "sint", "arg": 12},
             "loc": Location(self.filename, 19, 13)},
        ])
        self.assertEqual(add["format"], "R")
        self.assertEqual(add["encoding"], {"opcode": 10, "rd": "rd", "k": "k"})
        self.assertEqual(add["assembly"], "add {rd}, {k}")
        self.assertEqual(add["semantics"]["source"], " write(rd,read(rd) +% sext<XLEN>(k)); ")
        self.assertEqual(nop["encoding"], {"opcode": 0, "k": -2})
        self.assertEqual(nop["assembly"], "nop")
        self.assertEqual(nop["requires"], [])
        self.assertEqual(nop["operands"], [])
        self.assertIsNone(nop["semantics"])

    def test_raw_semantics_balances_braces_without_parsing(self):
        body = '\r\n    /* } */ { foo("} \\" {"); } // }\r\n    # }\r\n  '
        source = (
            "/* leading comment */\r\narchitecture CPU {\r\n"
            "  instruction NOP {\r\n"
            "    semantics {" + body + "}\r\n"
            "    encoding F {} assembly \"nop\";\r\n"
            "  }\r\n}\r\n"
        )
        add = parse(source, self.filename)["declarations"][0]
        self.assertEqual(add["semantics"], {"source": body, "loc": Location(self.filename, 4, 16)})

    def test_empty_semantics_is_distinct_from_missing_semantics(self):
        root = self.parse('module M { instruction NOP { semantics {} assembly "nop"; encoding F {} } }')
        self.assertEqual(root["declarations"][0]["semantics"], {
            "source": "", "loc": Location(self.filename, 1, 41),
        })

    def test_retains_duplicate_top_level_declarations_for_validation(self):
        root = self.parse("""
            architecture CPU {
                endian = little;
                endian = big;
                feature I;
                feature I;
            }
            """)
        self.assertEqual([node["name"] for node in root["declarations"]], ["endian", "endian", "I", "I"])
        self.assertEqual(root["declarations"][1]["loc"], Location(self.filename, 3, 5))

    def test_reports_duplicate_members_instead_of_overwriting(self):
        cases = [
            ("register_file R { count=2; count=3; }", "duplicate register_file member 'count'"),
            ("register_file R { alias n=x[0]; alias n=x[1]; }", "duplicate register alias 'n'"),
            ("register_class R { members=F.*; members=F.*; }", "duplicate register_class member"),
            ("format F: bits<8> { field x: bits<8> @[7:0]; field x: bits<8> @[7:0]; }",
             "duplicate format field 'x'"),
            ("instruction I { operands { in x:R; out x:R; } }", "duplicate operand 'x'"),
            ("instruction I { encoding F { x=0; x=1; } }", "duplicate encoding field 'x'"),
            ('instruction I { assembly "a"; assembly "b"; }', "duplicate instruction member 'assembly'"),
        ]
        for declaration, message in cases:
            with self.subTest(declaration=declaration):
                with self.assertRaisesRegex(ArchonError, message):
                    self.parse("module M { " + declaration + " }")

    def test_unsupported_declaration_has_location(self):
        with self.assertRaisesRegex(ArchonError, "unsupported declaration 'foreach'") as caught:
            self.parse("""
                module M {
                    // Bulk generation is not supported by this frontend.
                    foreach Width in [8, 16] {}
                }
                """)
        self.assertEqual(caught.exception.location, Location(self.filename, 3, 5))

    def test_unsupported_members_and_types(self):
        for declaration, message in [
            ("instruction N { selection {} }", "unsupported instruction member 'selection'"),
            ("type Wide = vec<8>;", "unsupported parameterized type 'vec'"),
            ("instruction N { encoding F { x = custom(rd); } }", "unsupported encoding call 'custom'"),
            ('instruction N { assembly { syntax "nop"; dialect GNU; } }',
             "unsupported assembly member 'dialect'"),
        ]:
            with self.subTest(declaration=declaration), self.assertRaisesRegex(ArchonError, message):
                self.parse("module M { " + declaration + " }")

    def test_reports_missing_instruction_members(self):
        for body, missing in [('assembly "nop";', "encoding"), ("encoding F {}", "assembly")]:
            with self.subTest(missing=missing):
                with self.assertRaisesRegex(ArchonError, f"instruction requires '{missing}'") as caught:
                    self.parse(f"module M {{\n    instruction NOP {{ {body} }}\n}}")
                self.assertEqual(caught.exception.location, Location(self.filename, 2, 5))

    def test_malformed_tokens_have_locations(self):
        cases = [
            (
                "architecture CPU {\n  /* open",
                "unterminated block comment",
                Location(self.filename, 2, 3),
            ),
            (
                'architecture CPU {\n  description = "open\n}',
                "unterminated string literal",
                Location(self.filename, 2, 17),
            ),
            (
                "architecture CPU {\n  address_width = 0b12;\n}",
                "invalid integer literal '0b12'",
                Location(self.filename, 2, 19),
            ),
            (
                "architecture CPU {\n  instruction NOP { semantics { call(); ",
                "unterminated semantics block",
                Location(self.filename, 2, 31),
            ),
        ]
        for source, message, loc in cases:
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message) as caught:
                self.parse(source)
            self.assertEqual(caught.exception.location, loc)


if __name__ == "__main__":
    unittest.main()
