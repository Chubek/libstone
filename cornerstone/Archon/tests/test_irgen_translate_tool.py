"""Exercise the standalone translator on real Aurocks/ASDL and generated ANF."""

import ast
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, pipeline_stages
from archon.irgen.translate_tool import emit


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "examples" / "translate"
TEMP = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None


class TranslateToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        db = compile_file(ROOT / "stdspec" / "riscv" / "rv32i.arch")
        cls.anf = generate_ir("anf", [db], options={"oracle": False})
        cls.gated = emit({"ir": "gated"})["Translate.py"]

    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="test-archon-translate-", dir=TEMP)
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.script = self.root / "Translate.py"
        self.script.write_text(self.anf["Translate.py"], encoding="utf-8")
        for name in ("Lang.g", "Lang.asdl", "MySource.lang"):
            shutil.copyfile(FIXTURE / name, self.root / name)

    def write(self, name, text):
        (self.root / name).write_text(text, encoding="utf-8")

    def read(self, name):
        return (self.root / name).read_text(encoding="utf-8")

    def parts(self):
        inline, marker, schema = self.read("Lang.asdl").partition("-- archon-translate-end\n")
        self.assertTrue(marker)
        mapping = "\n".join(line[3:] for line in inline.splitlines()[1:])
        return schema.lstrip("\n"), mapping

    def run_translate(self, *, out="MyProgram.i", mapping=None):
        args = [
            sys.executable, "-I", self.script.name,
            "--grammar", "Lang.g", "--absyn", "Lang.asdl", "--out", out,
        ]
        if mapping is not None:
            args.extend(("--mapping", mapping))
        return subprocess.run(
            [*args, "MySource.lang"], cwd=self.root, text=True, capture_output=True,
            env={**os.environ, "PYTHONPATH": str(self.root)}, timeout=30,
        )

    def require_perl(self):
        if not shutil.which("perl"):
            self.skipTest("Perl unavailable")

    def assert_rejected(self, diagnostic, **kwargs):
        destination = kwargs.pop("out", "MyProgram.i")
        self.write(destination, "keep this output\n")
        result = self.run_translate(out=destination, **kwargs)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(diagnostic, result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertEqual(self.read(destination), "keep this output\n")
        self.assertEqual(list(self.root.glob(".archon-translate-*")), [])

    def test_auto_discovery_and_verbatim_vendoring(self):
        self.assertIn("archon.irgen.translate_tool", {stage.__name__ for stage in pipeline_stages()})
        code = self.anf["Translate.py"]
        self.assertEqual(code, emit({"ir": "anf"})["Translate.py"])
        self.assertIn("Copyright (c) 2026 Chubak Bidpaa", code)
        self.assertIn("Permission is hereby granted", code)
        embedded = {
            line.targets[0].id: ast.literal_eval(line.value)
            for line in ast.parse(code).body
            if isinstance(line, ast.Assign) and isinstance(line.targets[0], ast.Name)
            and line.targets[0].id in {"ASDL_SOURCE", "AUROCKS_SOURCE"}
        }
        self.assertEqual(embedded["ASDL_SOURCE"], (ROOT.parent / "ASDL/scripts/asdl.pl").read_bytes())
        self.assertEqual(embedded["AUROCKS_SOURCE"], (ROOT.parent / "Aurocks/scripts/aurocks.pl").read_bytes())

    def test_translate_and_decode_with_generated_c99(self):
        self.require_perl()
        result = self.run_translate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.root / "Lang.translate.json").exists())
        text = self.read("MyProgram.i")
        self.assertIn('IR_PROGRAM_BEGIN("MiniCalc");', text)
        self.assertIn("IR_PRIM_MUL", text)
        self.assertEqual(text.count("IR_PRIM_SUB"), 2)
        self.assertIn("IR_ARG_BOOL(1);", text)
        second = self.run_translate(out="Another Program.i")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(self.read("Another Program.i"), text)
        self.assertFalse((self.root / "AUROCKS_STRPOOL").exists())
        self.assertEqual(list(self.root.glob("*.pl")), [])

        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("C99 compiler unavailable")
        for name in ("IR.c", "IR.h"):
            self.write(name, self.anf[name])
        shutil.copyfile(FIXTURE / "decode.c", self.root / "decode.c")
        checked = subprocess.run(
            [*compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
             '-DARCHON_IR_PROGRAM_FILE="MyProgram.i"', "IR.c", "decode.c", "-o", "decode"],
            cwd=self.root, capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(checked.returncode, 0, checked.stderr)
        decoded = subprocess.run(
            [str(self.root / "decode")], cwd=self.root, text=True, capture_output=True,
            timeout=30,
        )
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertIn("program MiniCalc", decoded.stdout)
        self.assertIn("function @compute(%x: i32, %y: i32) -> i32", decoded.stdout)
        self.assertIn("%archon_v_3: i32 = add %x, 0x2", decoded.stdout)
        self.assertIn("%archon_v_4: i32 = mul %archon_v_3, %y", decoded.stdout)
        self.assertIn("%archon_v_6: i32 = sub %scaled, 0x3", decoded.stdout)
        self.assertIn("%archon_v_7: i32 = sub %archon_v_6, 0x1", decoded.stdout)
        self.assertIn("function @is_two(%x: i32) -> bool", decoded.stdout)
        self.assertIn("function @ready() -> bool", decoded.stdout)

    def test_grammar_and_mapping_are_inputs_not_builtin_language(self):
        self.require_perl()
        text = self.read("Lang.g")
        self.write("Lang.g", text.replace(r"/let\b/", r"/bind\b/")
                   .replace("/[0-9]+/", "/[0-9]|[0-9]+/ %dprec 3"))
        self.write("MySource.lang", (
            "module Alternate; fn main(trueish: i32) -> i32 { "
            "bind x = trueish + 42; return x; }\n"
        ))
        self.assert_rejected("mapping rhs does not match the grammar")
        _, mapping = self.parts()
        mapping = mapping.replace(r"/let\\b/", r"/bind\\b/").replace("/[0-9]+/", "/[0-9]|[0-9]+/")
        self.write("Another Mapping.json", mapping)
        result = self.run_translate(mapping="Another Mapping.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("IR_ARG_INT(IR_TYPE_I32, UINT64_C(0x2a));", self.read("MyProgram.i"))
        self.assertIn('"trueish", IR_TYPE_I32', self.read("MyProgram.i"))
        schema, _ = self.parts()
        self.write("Lang.translate.json", mapping)
        self.write("Lang.asdl", schema)
        result = self.run_translate()
        self.assertEqual(result.returncode, 0, result.stderr)
        explicit = json.loads(mapping)
        explicit["lowering"]["Add"]["op"] = "sub"
        self.write("Another Mapping.json", json.dumps(explicit))
        result = self.run_translate(mapping="Another Mapping.json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("IR_PRIM_SUB", self.read("MyProgram.i"))
        self.assertNotIn("IR_PRIM_ADD", self.read("MyProgram.i"))

    def test_rejects_invalid_or_native_grammar_before_publication(self):
        self.require_perl()
        original = self.read("Lang.g")
        for grammar, diagnostic in (
            (original.replace("name: /[[:alpha:]_][[:alnum:]_]*/;",
                              "name: /[[:alpha:]_][[:alnum:]_]*/ { $$ = $1; };"),
             "native grammar actions and C sections are not supported"),
            ("%{ static int untrusted; %}\n" + original, "native grammar actions"),
            (original + "int x;\n", "unsupported grammar epilogue"),
            (original.replace("%start program", "%start program\n%left '+'"),
             "unsupported grammar directive %left"),
            (original.replace("/[0-9]+/", "/[0-9]( /"), "bad regular expression"),
            (original.replace("name: /[[:alpha:]_][[:alnum:]_]*/;", "name: missing;"),
             "unknown grammar symbol [missing]"),
            (original.replace("name: /[[:alpha:]_][[:alnum:]_]*/;", "name: other;\nother: name;"),
             "indirect left recursion"),
            (original.replace("/[0-9]+/", "/(abc{512}){512}/"), "expands to too many states"),
        ):
            with self.subTest(diagnostic=diagnostic):
                self.write("Lang.g", grammar)
                self.assert_rejected(diagnostic)

    def test_rejects_invalid_schema_and_mapping(self):
        self.require_perl()
        schema, original = self.parts()
        self.write("Lang.translate.json", original)
        self.write("Lang.asdl", schema.replace("Integer(int value)", "Integer(missing value)"))
        self.assert_rejected("undefined type 'missing'")
        self.write("Lang.asdl", "%{\nstatic int untrusted;\n%}\n%%\n" + schema)
        self.assert_rejected("native ASDL C sections")
        self.write("Lang.asdl", "%schema forbidden-write.json\n%%\n" + schema)
        self.assert_rejected("schema directives are not supported")
        self.assertFalse((self.root / "forbidden-write.json").exists())
        self.write("Lang.asdl", schema)
        self.assert_rejected("an explicit semantic mapping is required", mapping="Missing.json")
        mapping = json.loads(original)
        mapping["productions"]["statement"]["alternatives"][0]["build"]["fields"]["binding"] = 6
        self.write("Lang.translate.json", json.dumps(mapping))
        self.assert_rejected("has ASDL type expr, expected identifier")
        self.write("Lang.translate.json", original.replace('"version": 1,', '"version": 1, "version": 1,'))
        self.assert_rejected("duplicate JSON key 'version'")
        self.write("Lang.translate.json", original.replace('"op": "add"', '"op": "emit C text"'))
        self.assert_rejected("unknown lowering operation")

    def test_inline_mapping_block_must_be_closed(self):
        schema = self.read("Lang.asdl")
        self.write("Lang.asdl", schema[:schema.index("-- archon-translate-end")])
        self.assert_rejected("unterminated inline mapping")

    def test_synthetic_names_do_not_collide_with_source_names(self):
        self.require_perl()
        self.write("MySource.lang", """
            module Names;
            fn archon_fn_2(archon_v_1: i32) -> i32 { return archon_v_1 + 2; }
            fn second() -> i32 { return 0; }
        """)
        result = self.run_translate()
        self.assertEqual(result.returncode, 0, result.stderr)
        text = self.read("MyProgram.i")
        self.assertIn('IR_FUNCTION_BEGIN(archon_fn_1, "archon_fn_2", IR_TYPE_I32);', text)
        self.assertIn('IR_FUNCTION_BEGIN(archon_fn_3, "second", IR_TYPE_I32);', text)
        self.assertIn('IR_PARAM(archon_v_2, "archon_v_1", IR_TYPE_I32);', text)

    def test_rejects_syntax_and_type_errors_without_overwriting_output(self):
        self.require_perl()
        for text, diagnostic in (
            ("module X; fn f() -> i32 { return 2 }", "source does not match the grammar"),
            ("module X; fn f() -> i32 { return 2; } extra", "unexpected trailing source"),
            ("module X;\nfn f() -> i32 { return wrong; }", "MySource.lang:2:24: unknown name 'wrong'"),
            ("module X; fn f() -> i32 { return 2147483648; }", "does not fit i32"),
            ("module X; fn f() -> i32 { return 99999999999999999999; }",
             "invalid terminal value for ASDL int"),
            ("module X; fn f() -> i32 { return true; }", "return has type bool, expected i32"),
            ("module X; fn f() -> i32 { let a: bool = 1; return a; }", "initializer type"),
            ("module X; fn f(a: i32) -> i32 { let a = 0; return a; }", "duplicate binding 'a'"),
            ("module X; fn f() -> i32 { return 1; let b = 2; }", "statement after return"),
            ("module X; fn f() -> i32 {}", "must end in return"),
            ("module X; fn f() -> i32 { return " + "(" * 200 + "1" + ")" * 200 + "; }",
             "nested too deeply"),
        ):
            with self.subTest(diagnostic=diagnostic):
                self.write("MySource.lang", text)
                self.assert_rejected(diagnostic)

    def test_output_may_not_alias_input_and_other_ir_is_explicitly_unsupported(self):
        original = self.read("MySource.lang")
        result = self.run_translate(out="MySource.lang")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("output aliases an input", result.stderr)
        self.assertEqual(self.read("MySource.lang"), original)
        if hasattr(os, "symlink"):
            (self.root / "Alias.i").symlink_to(self.root / "MySource.lang")
            result = self.run_translate(out="Alias.i")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("not a symlink", result.stderr)
            self.assertEqual(self.read("MySource.lang"), original)
        self.script.write_text(self.gated, encoding="utf-8")
        self.assert_rejected("representation 'gated' is not supported")


if __name__ == "__main__":
    unittest.main()
