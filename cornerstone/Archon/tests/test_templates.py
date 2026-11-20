"""ArchDef parsing, expansion, validation, and integration tests."""

import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError, Location
from archon.frontend import parse
from archon.runtime import Target


_BASE = """
    endian = little;
    address_width = Width;
    parameter Width: nat in {16, 32} = 16;
    feature Base = enabled;
    register_file Registers { count = 4; width = Width; register x[0..3]; }
    register_class GPR { members = Registers.*; }
    format R: bits<16> {
        field opcode: bits<7> @ [6:0];
        field rd: bits<2> @ [8:7];
        field rs1: bits<2> @ [10:9];
        field rs2: bits<2> @ [12:11];
        field tag: bits<3> @ [15:13];
    }
"""

_BINARY = """
    template Binary<Name: string, Opcode: bits<7>> {
        instruction self {
            requires feature(Base);
            operands { out rd: GPR; in rs1: GPR; in rs2: GPR; }
            encoding R {
                opcode = Opcode;
                rd = encode_register(rd);
                rs1 = rs1;
                rs2 = rs2;
                tag = 0;
            }
            assembly "{Name} {rd}, {rs1}, {rs2}";
            semantics { let Name = read(rs1); write(rd, Name +% read(rs2)); }
        }
    }
"""

_ADD = 'def ADD = Binary<"add", 0x33>;'
_ADD_DIRECT = """
    instruction ADD {
        requires feature(Base);
        operands { out rd: GPR; in rs1: GPR; in rs2: GPR; }
        encoding R { opcode = 0x33; rd = rd; rs1 = rs1; rs2 = rs2; tag = 0; }
        assembly "add {rd}, {rs1}, {rs2}";
        semantics { let Name = read(rs1); write(rd, Name +% read(rs2)); }
    }
"""

_IMMEDIATE = """
    format I: bits<8> {
        field opcode: bits<3> @ [7:5];
        field rd: bits<2> @ [4:3];
        field k: sint<3> @ [2:0];
    }
    template Set<Name: string, Opcode: nat, Width: nat> {
        instruction self {
            operands { out rd: GPR; in k: imm<sint<Width>>; }
            encoding I { opcode = Opcode; rd = rd; k = k; }
            assembly { syntax "{Name} {rd}, {k}"; }
            semantics { write(rd, sext<Width>(k)); }
        }
    }
"""


class TemplateTests(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.directory = Path(temp.name)

    def write(self, relative, source):
        path = self.directory / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")
        return path

    def compile(self, body, **options):
        path = self.write("root.arch", "architecture Example {\n" + _BASE + body + "\n}")
        return compile_file(path, **options)

    def test_parse_parameter_kinds_literal_arguments_and_reference_location(self):
        parsed = parse("module Ops {" + _BINARY + _ADD + "}", "ops.arch")
        template, definition = parsed["declarations"]
        self.assertEqual((template["kind"], template["name"]), ("template", "Binary"))
        self.assertEqual(
            [(param["name"], param["spec"]) for param in template["parameters"]],
            [("Name", {"kind": "string"}), ("Opcode", {"kind": "bits", "width": 7})],
        )
        self.assertEqual(template["body"]["encoding"]["opcode"], {
            "parameter": "Opcode", "loc": Location("ops.arch", 7, 26),
        })
        self.assertEqual(template["body"]["encoding"]["rd"], "rd")
        self.assertEqual((definition["kind"], definition["name"], definition["template"]),
                         ("def", "ADD", "Binary"))
        self.assertEqual([arg["value"] for arg in definition["arguments"]], ["add", 0x33])

    def test_diamond_imports_forward_references_and_deterministic_asir(self):
        self.write("isa/shared.arch", "module Shared {" + _BINARY + "}")
        self.write("isa/left.arch", 'module Left { def OTHER = Binary<"other", 0x34>; import shared; }')
        self.write("isa/right.arch", "module Right { import shared; }")
        db = self.compile(_ADD + "import isa.left; import isa.right;")
        reordered = self.compile("import isa.right; import isa.left;" + _ADD)
        self.assertEqual(json.dumps(db), json.dumps(reordered))
        self.assertEqual([i["id"] for i in db["instructions"]], ["Example.ADD", "Example.OTHER"])
        self.assertEqual(db["instructions"][0], self.compile(_ADD_DIRECT)["instructions"][0])

        target = Target(db)
        asm = "add x1, x2, x3"
        inst = target.assemble(asm)
        raw = target.encode(*inst)
        self.assertEqual(raw, bytes.fromhex("b31c"))
        self.assertEqual(target.disassemble(raw), asm)
        self.assertEqual(target.decode(raw), inst)
        self.assertEqual(target.execute(*inst, [0, 0, 65535, 3]), [0, 2, 65535, 3])

    def test_nat_encoding_and_immediate_width_without_substituting_semantics(self):
        target = Target(self.compile(
            _IMMEDIATE + 'def SET = Set<"set", 5, 3>;', parameters={"Width": 32},
        ))
        inst = target.assemble("set x1, -1")
        self.assertEqual(target.encode(*inst), b"\xaf")
        self.assertEqual(target.execute(*inst, [0, 0, 0, 0]), [0, 0xFFFFFFFF, 0, 0])
        for width, message in (
            (0, "sint width must be between 1 and 4096"),
            (4, "does not match the width and signedness"),
        ):
            with self.subTest(width=width), self.assertRaisesRegex(ArchonError, message):
                self.compile(_IMMEDIATE + f'def SET = Set<"set", 5, {width}>;')

    def test_template_syntax_is_bounded(self):
        params = ", ".join(f"P{i}: nat" for i in range(33))
        args = ", ".join("1" for _ in range(33))
        for source, message in (
            ("template T<X> {}", "expected ':'"),
            ("template T<X: type> {}", "expected template parameter type"),
            ("template T<X: bits<Width>> {}", "expected an integer"),
            ("template T<X: nat, X: string> {}", "duplicate template parameter 'X'"),
            ("template T<> { instruction NOT_SELF {} }", "expected 'self'"),
            ("template T<> { def LOOP = T<>; }", "expected 'instruction'"),
            (f"template T<{params}> {{}}", "at most 32 parameters"),
            (f"def X = T<{args}>;", "at most 32 arguments"),
            ("def X = T<add>;", "expected a template argument"),
            ("def X = T<T<>>;", "expected a template argument"),
            ("def X = T<0b" + "0" * 4097 + ">;", "exceeds 4096 digits"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                parse("module M {" + source + "}", "test.arch")

    def test_rejects_unknown_wrong_arity_and_argument_types_at_use_site(self):
        for source, message in (
            ('def ADD = Unknown<"add", 0x33>;', "unknown instruction template 'Unknown'"),
            ('def ADD = Binary<"add">;', "expects 2 argument.*got 1"),
            ('def ADD = Binary<"add", 0x33, 0>;', "expects 2 argument.*got 3"),
            ("def ADD = Binary<4, 0x33>;", "Name.*must be a string literal"),
            ('def ADD = Binary<"add", -1>;', "Opcode.*must be an integer fitting"),
            ('def ADD = Binary<"add", 128>;', r"Opcode.*must be an integer fitting bits<7>"),
            ('def ADD = Binary<"add", "0x33">;', "Opcode.*must be an integer fitting"),
            ('def ADD = Binary<"add{rd}", 0x33>;', "cannot contain braces"),
            (r'def ADD = Binary<"add\n", 0x33>;', "must be printable"),
        ):
            with self.subTest(source=source), self.assertRaisesRegex(ArchonError, message) as caught:
                self.compile(_BINARY + "\n" + source)
            loc = caught.exception.location
            self.assertEqual(loc.filename, str(self.directory / "root.arch"))
            self.assertEqual(self.directory.joinpath("root.arch").read_text().splitlines()[loc.line - 1],
                             source)
            self.assertGreater(loc.column, len("def ADD = "))
        for arg, message in (
            ("-1", "must be a natural number"),
            ("0x1" + "0" * 1024, "exceeds the 4096-bit integer limit"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                self.compile(_IMMEDIATE + f'def SET = Set<"set", {arg}, 3>;')

    def test_rejects_invalid_template_parameter_uses_even_without_instances(self):
        for original, replacement, message in (
            ("bits<7>", "bits<0>", "bit width must be between 1 and 4096"),
            ("bits<7>", "bits<4097>", "bit width must be between 1 and 4096"),
            ("opcode = Opcode", "opcode = Name", "must have type 'bits<N>' or 'nat'"),
            ("{Name}", "{Opcode}", "must have type 'string'"),
            ("{Name}", "{{Name}}", "invalid assembly placeholder"),
            ("out rd: GPR", "out Name: GPR", "conflicts with a template parameter"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                self.compile(_BINARY.replace(original, replacement))
        with self.assertRaisesRegex(ArchonError, "must have type 'nat'"):
            self.compile(_IMMEDIATE.replace("sint<Width>", "sint<Opcode>")
                         .replace("Opcode: nat", "Opcode: bits<3>"))

    def test_expanded_instructions_use_normal_collision_and_feature_checks(self):
        self.assertEqual(self.compile(_BINARY + _ADD, features=[])["instructions"], [])
        for declarations, message in (
            (_ADD + _ADD_DIRECT, "duplicate declaration 'ADD'"),
            (_ADD + "feature Binary;", "duplicate declaration 'Binary'"),
            (_ADD + 'def OTHER = Binary<"other", 0x33>;',
             "overlapping encodings for instructions 'ADD' and 'OTHER'"),
            (_ADD + 'def OTHER = Binary<"add", 0x34>;', "duplicate assembly mnemonic 'add'"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                self.compile(_BINARY + declarations)
        # encode_register's argument is not substituted; validation still runs with Base disabled.
        with self.assertRaisesRegex(ArchonError, "unknown operand 'Opcode'"):
            self.compile(_BINARY.replace("encode_register(rd)", "encode_register(Opcode)") + _ADD,
                         features=[])

    def test_imported_body_errors_report_body_and_instantiation_sites(self):
        path = self.write("ops.arch", "module Ops {" + _BINARY.replace("bits<7>", "bits<8>") + "}")
        with self.assertRaisesRegex(
            ArchonError, r"constant 128 does not fit encoding field 'opcode'.*instantiating 'ADD' at .*root\.arch",
        ) as caught:
            self.compile('import ops; def ADD = Binary<"add", 128>;', features=[])
        self.assertEqual(caught.exception.location.filename, str(path))

    def test_caps_expansion_count_and_interpolation_size(self):
        defs = "\n".join(f'def I{i:04d} = Binary<"add", 1>;' for i in range(4097))
        with self.assertRaisesRegex(ArchonError, "at most 4096 template definitions"):
            self.compile(_BINARY + defs)
        text = "x" * 16385
        with self.assertRaisesRegex(ArchonError, "exceeds 16384 characters"):
            self.compile(_BINARY + f'def ADD = Binary<"{text}", 1>;')
        text = "x" * 9000
        with self.assertRaisesRegex(ArchonError, "expanded assembly exceeds 16384 characters"):
            self.compile(_BINARY.replace("{Name}", "{Name}{Name}") +
                         f'def ADD = Binary<"{text}", 1>;')

    def test_zero_arity_template(self):
        empty = """
            template Zero<> {
                instruction self {
                    encoding R { opcode = 0; rd = 0; rs1 = 0; rs2 = 0; tag = 0; }
                    assembly "zero";
                }
            }
            def ZERO = Zero<>;
        """
        self.assertEqual(self.compile(empty)["instructions"][0]["id"], "Example.ZERO")


if __name__ == "__main__":
    unittest.main()
