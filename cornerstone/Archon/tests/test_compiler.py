"""Compiler tests using independent architecture source graphs."""

import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError


_SETTINGS = """
    parameter XLEN: nat in {16, 32} = 16;
    address_width = XLEN;
    endian = little;
"""

_REGISTERS = """
    type Word = bits<XLEN>;
    type Immediate = SignedOffset;
    type SignedOffset = sint<3>;
    register_file Registers {
        count = 4;
        width = XLEN;
        register x[0..3];
        zero = 0;
        alias zero = x[0];
    }
    register_class GPR { members = Registers.*; }
"""

_FORMAT = """
    format Tiny: bits<8> {
        field opcode: bits<3> @ [7:5];
        field rd: bits<2> @ [4:3];
        field disp: Immediate @ [2:0];
    }
"""

_INSTRUCTION = """
    instruction SET {
        operands { out rd: GPR; in imm: imm<Immediate>; }
        encoding Tiny { opcode = 5; rd = encode_register(rd); disp = imm; }
        assembly "set {rd}, {imm}";
        semantics { write(rd, sext<XLEN>(imm)); }
    }
"""


class CompilerTest(unittest.TestCase):
    def setUp(self):
        temp = TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.directory = Path(temp.name)

    def write(self, relative, source):
        path = self.directory / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")
        return path

    def architecture(self, body):
        return self.write("root.arch", "architecture Example {\n" + body + "\n}\n")

    def compile(self, body, **configuration):
        return compile_file(self.architecture(body), **configuration)

    def test_relative_diamond_imports_and_stable_normalized_asir(self):
        self.write("isa/common.arch", "module Common {" + _REGISTERS + _FORMAT + "}")
        self.write("isa/left.arch", "module Left { import common;" + _INSTRUCTION + "}")
        self.write("isa/other/right.arch", "module Right { type Extra = bits<XLEN>; }")
        self.write("isa/right.arch", "module Bridge { import common; import other.right; }")

        db = self.compile("import isa.left;\nimport isa.right;\n" + _SETTINGS, parameters={"XLEN": 32})
        reordered = self.compile("import isa.right;\n" + _SETTINGS + "\nimport isa.left;",
                                 parameters={"XLEN": 32})
        self.assertEqual(json.dumps(db), json.dumps(reordered))
        self.assertEqual(db["parameters"], {"XLEN": 32})
        self.assertEqual(db["address_width"], 32)
        self.assertEqual(db["registers"], {
            "name": "Registers", "prefix": "x", "count": 4, "width": 32,
            "zero": 0, "aliases": {"zero": 0},
        })
        self.assertEqual(db["types"]["Extra"], {"kind": "bits", "width": 32})
        self.assertEqual(db["types"]["Immediate"], {"kind": "sint", "width": 3})
        self.assertEqual(len(db["instructions"]), 1)
        instruction = db["instructions"][0]
        self.assertEqual(
            {key: value for key, value in instruction.items() if key != "semantics"},
            {
                "id": "Example.SET", "name": "SET", "width": 8, "mask": 0xE0, "value": 0xA0,
                "operands": [
                    {"name": "rd", "direction": "out", "kind": "reg", "width": 2, "signed": False},
                    {"name": "imm", "direction": "in", "kind": "imm", "width": 3, "signed": True},
                ],
                "fields": [
                    {"operand": "imm", "lsb": 0, "width": 3},
                    {"operand": "rd", "lsb": 3, "width": 2},
                ],
                "assembly": "set {rd}, {imm}",
            },
        )
        self.assertEqual(instruction["semantics"]["word_width"], 32)

    def test_cycle_and_missing_module_report_the_import_site(self):
        root = self.architecture("import isa.a;")
        self.write("isa/a.arch", "module A {\n    import b;\n}")
        last = self.write("isa/b.arch", "module B {\n    import a;\n}")
        with self.assertRaisesRegex(ArchonError, r"import cycle: .*a\.arch -> .*b\.arch -> .*a\.arch") as caught:
            compile_file(root)
        self.assertEqual(caught.exception.location.filename, str(last.resolve()))
        self.assertEqual((caught.exception.location.line, caught.exception.location.column), (2, 5))

        self.write("isa/b.arch", "module B { import missing; }")
        with self.assertRaisesRegex(ArchonError, r"cannot read .*isa/missing\.arch") as caught:
            compile_file(root)
        self.assertEqual(caught.exception.location.filename, str(last.resolve()))

    def test_entrypoint_and_import_kinds(self):
        root = self.write("root.arch", "module Root {}")
        with self.assertRaisesRegex(ArchonError, "expected an architecture declaration"):
            compile_file(root)
        self.write("other.arch", "architecture Other {}")
        with self.assertRaisesRegex(ArchonError, "expected a module declaration"):
            self.compile("import other;")

    def test_parameters_validate_domains_and_overrides(self):
        body = _SETTINGS + _REGISTERS
        self.assertEqual(self.compile(body)["parameters"], {"XLEN": 16})
        for parameters, message in (
            ({"Other": 16}, "unknown parameter"),
            ({"XLEN": True}, "outside its domain"),
            ({"XLEN": 64}, "outside its domain"),
        ):
            with self.subTest(parameters=parameters), self.assertRaisesRegex(ArchonError, message):
                self.compile(body, parameters=parameters)
        with self.assertRaisesRegex(ArchonError, "default .* outside"):
            self.compile(body.replace("{16, 32}", "{32}"), parameters={"XLEN": 32})
        with self.assertRaisesRegex(ArchonError, "domain of natural numbers"):
            self.compile(body.replace("{16, 32}", "{16, -1}"))

    def test_feature_replacement_and_dependency_validation(self):
        guarded = _INSTRUCTION.replace("instruction SET {", "instruction SET { requires feature(M);")
        body = _SETTINGS + _REGISTERS + _FORMAT + guarded + """
            feature I = enabled;
            feature M requires I;
            feature Z = enabled;
        """
        default = self.compile(body)
        self.assertEqual(default["features"], ["I", "Z"])
        self.assertEqual(default["instructions"], [])
        configured = self.compile(body, features={"M", "I"})
        self.assertEqual(configured["features"], ["I", "M"])
        self.assertEqual([i["name"] for i in configured["instructions"]], ["SET"])
        self.assertEqual(self.compile(body, features=[])["features"], [])
        for selection, message in (
            (["M"], "requires enabled feature.*I"),
            (["Y"], "unknown feature 'Y'"),
        ):
            with self.subTest(selection=selection), self.assertRaisesRegex(ArchonError, message):
                self.compile(body, features=selection)
        with self.assertRaisesRegex(ArchonError, "feature dependency cycle: A -> B -> A"):
            self.compile(body + "feature A requires B; feature B requires A;")
        with self.assertRaisesRegex(ArchonError, "unknown required feature 'Missing'"):
            self.compile(body + "feature A requires Missing;")
        with self.assertRaisesRegex(ArchonError, "requires enabled feature.*I"):
            self.compile(body.replace("feature I = enabled;", "feature I;")
                             .replace("feature M requires", "feature M = enabled requires"))

    def test_unknown_and_duplicate_names_and_type_cycles(self):
        self.write("types.arch", "module Types { type Word = uint<16>; }")
        body = _SETTINGS + _REGISTERS + _FORMAT + _INSTRUCTION
        for source, message in (
            (body + "import types;", "duplicate declaration 'Word'"),
            (body.replace("type SignedOffset = sint<3>;", "type SignedOffset = Immediate;"),
             "type alias cycle: Immediate -> SignedOffset -> Immediate"),
            (body.replace("type Immediate = SignedOffset;", "type Immediate = Missing;"),
             "unknown type 'Missing'"),
            (body.replace("out rd: GPR;", "out rd: Missing;"), "unknown register class"),
            (body.replace("Registers.*", "Missing.*"), "unknown register file"),
            (body.replace("encoding Tiny", "encoding Missing"), "unknown instruction format"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                self.compile(source)

    def test_register_model_constraints(self):
        body = _SETTINGS + _REGISTERS + _FORMAT + _INSTRUCTION
        for original, replacement, message in (
            ("x[0..3]", "x[1..4]", "contiguous and zero-based"),
            ("alias zero = x[0]", "alias zero = x[4]", "register alias .*out-of-range"),
            ("alias zero = x[0]", "alias x1 = x[0]", "conflicts with a numbered register"),
            ("zero = 0", "zero = 4", "zero register index .*out of range"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                self.compile(body.replace(original, replacement))
        with self.assertRaisesRegex(ArchonError, "cannot encode register operand 'rd'"):
            self.compile(body.replace("count = 4", "count = 5").replace("x[0..3]", "x[0..4]"))
        with self.assertRaisesRegex(ArchonError, "multiple register files"):
            self.compile(body + "register_file Extra { count = 1; width = 16; register y[0..0]; }")

    def test_field_ranges_and_encoding_bijection(self):
        body = _SETTINGS + _REGISTERS + _FORMAT + _INSTRUCTION
        for original, replacement, message in (
            ("[7:5]", "[8:6]", "bit range .*out of bounds"),
            ("bits<8> {", "bits<16> {", "does not cover all"),
            ("bits<8> {", "bits<7> {", "multiple of eight bits"),
            ("[4:3]", "[5:4]", "overlaps another format field"),
            ("bits<2> @ [4:3]", "bits<1> @ [4:3]", "type width does not match"),
            ("opcode = 5", "opcode = 8", "constant 8 does not fit"),
            ("opcode = 5", "opcode = -1", "constant -1 does not fit"),
            ("disp = imm;", "absent = imm;", "unknown encoding field"),
            ("disp = imm;", "", "missing encoding field"),
            ("disp = imm;", "disp = missing;", "unknown operand"),
            ("disp = imm;", "disp = 0;", "missing encoding for operand"),
            ("field disp: Immediate", "field disp: uint<3>", "width and signedness"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                self.compile(body.replace(original, replacement))
        with self.assertRaisesRegex(ArchonError, "encoded by multiple"):
            self.compile(body.replace("disp = imm;", "disp = rd;")
                             .replace("field disp: Immediate", "field disp: bits<3>"))

        constant = self.compile(_SETTINGS + _REGISTERS + _FORMAT + """
            instruction FIXED {
                encoding Tiny { opcode = 0; rd = 0; disp = -4; }
                assembly "fixed";
            }
        """)["instructions"][0]
        self.assertEqual((constant["mask"], constant["value"]), (0xFF, 4))
        self.assertIsNone(constant["semantics"])

    def test_assembly_templates_validate_placeholders(self):
        body = _SETTINGS + _REGISTERS + _FORMAT + _INSTRUCTION
        for template, message in (
            ("set {rd}, {absent}", "unknown assembly placeholder"),
            ("set {rd}, {rd}, {imm}", "duplicate assembly placeholder"),
            ("set {rd}", "missing assembly placeholder"),
            ("set {rd:02}, {imm}", "invalid assembly placeholder"),
            ("set {rd}{imm}", "must have a separator"),
            ("set {rd},\\n{imm}", "one printable line"),
        ):
            with self.subTest(template=template), self.assertRaisesRegex(ArchonError, message):
                self.compile(body.replace("set {rd}, {imm}", template))

    def test_configuration_validates_disabled_instructions_but_excludes_their_conflicts(self):
        body = _SETTINGS + _REGISTERS + _FORMAT + _INSTRUCTION + "feature M;"
        other = _INSTRUCTION.replace(
            "instruction SET {", "instruction OTHER { requires feature(M);"
        ).replace("set {rd}", "other {rd}")
        self.assertEqual(len(self.compile(body + other)["instructions"]), 1)
        with self.assertRaisesRegex(ArchonError, "overlapping encodings"):
            self.compile(body + other, features=["M"])
        with self.assertRaisesRegex(ArchonError, "unknown assembly placeholder"):
            self.compile(body + other.replace("other {rd}", "other {bad}"))
        with self.assertRaisesRegex(ArchonError, "cannot read out operand"):
            self.compile(body + other.replace("sext<XLEN>(imm)", "read(rd)"))
        same_mnemonic = other.replace("other {rd}", "set {rd}").replace("opcode = 5", "opcode = 4")
        with self.assertRaisesRegex(ArchonError, "duplicate assembly mnemonic 'set'"):
            self.compile(body + same_mnemonic, features=["M"])

        long = """
            format Long: bits<16> { field value: bits<16> @ [15:0]; }
            instruction LONG {
                requires feature(M);
                encoding Long { value = 0; }
                assembly "long";
            }
        """
        self.assertEqual(len(self.compile(body + long)["instructions"]), 1)
        with self.assertRaisesRegex(ArchonError, "mixed instruction widths"):
            self.compile(body + long, features=["M"])

    def test_overlapping_encodings_with_different_masks(self):
        body = _SETTINGS + _REGISTERS + _FORMAT + _INSTRUCTION + """
            instruction FIXED {
                encoding Tiny { opcode = 5; rd = 0; disp = 0; }
                assembly "fixed";
            }
        """
        with self.assertRaisesRegex(ArchonError, "overlapping encodings"):
            self.compile(body)


if __name__ == "__main__":
    unittest.main()
