"""Standalone LaTeX/troff formatting of native and shared Archon .i streams."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from archon.irgen import EXPECTED_REPRESENTATIONS, pipeline_stages
from archon.irgen import prettyprint_tool


EXAMPLES = Path(__file__).resolve().parents[1] / "examples" / "ir-pipeline"
FIXTURES = EXAMPLES / "fixtures" / "v1"
ARTIFACTS = EXAMPLES / "expected" / "v1"

PROGRAMS = {
    "gated": ("""\
ARCHON_IR_PROGRAM_BEGIN(1, "gated")
ARCHON_IR_BLOCK(1, 1, "entry")
ARCHON_IR_NODE(1, 1, ARCHON_IR_OP_PARAMETER, ARCHON_IR_TYPE_UINT, 32, UINT64_C(0))
ARCHON_IR_NODE(2, 1, ARCHON_IR_OP_RISCV32_ADD, ARCHON_IR_TYPE_UINT, 32, UINT64_C(0))
ARCHON_IR_INPUT(2, 1, 0)
ARCHON_IR_INPUT(2, 1, 0)
ARCHON_IR_PROGRAM_END()
""", "ARCHON_IR_INPUT", "    ARCHON_IR_INPUT(2, 1, 0)"),
    "memory-ssa": ("""\
PROGRAM_DECLARE_BLOCK(0, "entry");
PROGRAM_ENTRY(0);
PROGRAM_BLOCK(0);
{
    const IR_value_id values[2] = {0, 1};
    ARCHON_IR_TARGET(2, IR_NO_VALUE, ARCHON_IR_OP_RISCV32_ADD,
                     IR_TYPE_BITS, 32, IR_NO_VALUE, 2, values);
}
IR_RETURN(2, IR_NO_VALUE);
""", "ARCHON_IR_TARGET", "const IR_value_id values[2] = {0, 1};"),
    "sea-of-nodes": ((FIXTURES / "sea-of-nodes" / "Program.i").read_text(encoding="utf-8"),
                     "ARCHON_IR_NODE", '  ARCHON_IR_ROOT(0u);'),
    "anf": ((FIXTURES / "anf" / "Program.i").read_text(encoding="utf-8"),
            "IR_LET_BEGIN", "        IR_ARG_VALUE(x);"),
    "cps": ("""\
CPS_PROGRAM_BEGIN(1)
CPS_TYPE(1, ARCHON_CPS_TYPE_BITS, 32, 0)
CPS_SIGNATURE_BEGIN(1, 1, 0, 0)
ARCHON_CPS_SIGNATURE_PARAMETER(1)
ARCHON_CPS_SIGNATURE_END()
CPS_PROGRAM_END()
""", "CPS_SIGNATURE_BEGIN", "    ARCHON_CPS_SIGNATURE_PARAMETER(1)"),
    "typed-tac": ("""\
IR_PROGRAM_BEGIN(1, 1, 1)
IR_BLOCK(0, "entry")
IR_OP(0, 0, 2, 0, 3, 32, 0, 1, 0, 4294967295, 4294967295, "entry")
IR_INPUT(0, 0, 0, 4294967295)
IR_PROGRAM_END()
""", "IR_OP", "    IR_INPUT(0, 0, 0, 4294967295)"),
    "untyped-tac": ((FIXTURES / "untyped-tac" / "Program.i").read_text(encoding="utf-8"),
                    "PROGRAM_BINARY", "PROGRAM_RETURN(2u);"),
    "supercombinators": ("""\
SC_PROGRAM_BEGIN();
SC_LITERAL(0, 1);
SC_CASE_BEGIN(1, 0);
ARCHON_IR_CASE_ARM(1, 0);
SC_CASE_END();
SC_ENTRY(1);
SC_PROGRAM_END();
""", "SC_CASE_BEGIN", "    ARCHON_IR_CASE_ARM(1, 0);"),
}


class PrettyPrintTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="archon prettyprint ")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.script = self.root / "PrettyPrint.py"
        self.script.write_text(prettyprint_tool.emit({})["PrettyPrint.py"], encoding="utf-8")

    def run_tool(self, *args, source=None, ok=True):
        result = subprocess.run(
            [sys.executable, "-I", str(self.script), *map(str, args)],
            cwd=self.root, input=source, capture_output=True, encoding="utf-8", timeout=10,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertNotIn("Traceback", result.stderr)
        return result

    def test_registry_and_all_native_representations(self):
        self.assertIn(prettyprint_tool, pipeline_stages())
        self.assertEqual(set(PROGRAMS), set(EXPECTED_REPRESENTATIONS))
        script = self.script.read_text(encoding="utf-8")
        for form, (source, anchor, troff_line) in PROGRAMS.items():
            with self.subTest(form=form):
                self.assertEqual(prettyprint_tool.emit({"ir": form}), {"PrettyPrint.py": script})
                latex = self.run_tool("--format", "latex", "-", source=source).stdout
                self.assertIn(r"\documentclass{article}", latex)
                self.assertIn(anchor.replace("_", r"\_") + "(", latex)
                self.assertTrue(latex.endswith("\\end{document}\n"))
                troff = self.run_tool("--format", "troff", "-", source=source).stdout
                self.assertIn(f"{anchor}(", troff)
                self.assertIn(r"\&" + troff_line, troff)
                self.assertTrue(troff.endswith(".fi\n"))

    def test_existing_shared_fragments(self):
        fragments = {
            "pipeline/MIR.i": "    ARCHON_MIR_USE(",
            "scheduler/MIR.i": "        MIR_INPUT_VALUE(",
            "selector/InstrMap.i": "    ARCHON_INSTR_OPERAND(",
            "selector/MIR.i": "    ARCHON_MIR_OPERAND(",
            "regalloc/MIR.i": "MIR_RANGE(",
            "regalloc/RegMap.i": "REGMAP_REGISTER(",
            "regalloc/Scheduled.i": "SCHEDULED_INSTRUCTION(",
            "regalloc/Regallocated.i": "  ARCHON_REGALLOCATED_ASSIGN(",
            "emitter/rv32i/Regallocated.i": "    REGALLOC_OPERAND_REGISTER(",
        }
        for path, anchor in fragments.items():
            with self.subTest(path=path):
                troff = self.run_tool(ARTIFACTS / path, "--format", "troff").stdout
                self.assertIn(r"\&" + anchor, troff)
                self.assertNotIn("#include", troff)
                self.assertNotIn("#endif", troff)

    def test_comments_strings_line_splices_and_c_fragments(self):
        source = r"""/* Header '' " /* remains a comment
   #define NOT_A_DIRECTIVE */
/* before directive */ #include "missing.h"
#define WRAPPER(x) \
    NEVER_DISPLAY(x)
#if ONE
IR_LET_NAMED_BEGIN /* gap */ (0, "this, ) // quote: \"",
            NESTED(1, (int[]){2, 3}), /* ignored (,*/ '(');
  IR_ARG_SYMBOL("joined\
across");
IR_LET_END(); IR_FOLLOWUP({(1), 2}, values[2]);
#else
{
   const int values[2] = {0, 1};
}
#endif
"""
        troff = self.run_tool("--format", "troff", "-", source=source).stdout
        self.assertIn("\\&/* Header '' \" /* remains a comment", troff)
        self.assertIn("#define NOT_A_DIRECTIVE", troff)
        self.assertIn(r'\&  0, "this, ) // quote: \e"", NESTED(1, (int[]){2, 3}),', troff)
        self.assertIn(" '(');", troff)
        self.assertIn(r'\&  IR_ARG_SYMBOL("joinedacross");', troff)
        self.assertIn(r"\&IR_FOLLOWUP({(1), 2}, values[2]);", troff)
        self.assertIn(r"\&const int values[2] = {0, 1};", troff)
        for skipped in ("missing.h", "NEVER_DISPLAY", "ignored (,", "#endif"):
            self.assertNotIn(skipped, troff)

    def test_hostile_control_sequences_remain_text(self):
        source = (
            'IR_TEXT("\\\\input{evil} % $ # & _ ^ ~ < > | ` {{title}} λ\\n");\n'
            '.so missing.roff\n'
            "'sy touch marker\n"
            r"\!raw \[.*] \X'anything'" + "\n"
            r"\end{document} ^^7b" + "\n"
        )
        title = ".so title_file\n'next \\! λ"
        troff = self.run_tool("--format", "troff", "--title", title, "-", source=source).stdout
        self.assertIn(r"\&.so missing.roff", troff)
        self.assertIn(r"\&'sy touch marker", troff)
        self.assertIn(r"\&\e!raw \e[.*] \eX'anything'", troff)
        self.assertIn(r"\&.so title_file\ex0a'next \e! \eu03bb", troff)
        self.assertNotIn("\n.so", troff)
        self.assertNotIn("\n'sy", troff)
        self.assertIn("{{title}}", troff)
        latex = self.run_tool("--format", "latex", "--title", title, "-", source=source).stdout
        self.assertIn(r"\textbackslash{}\textbackslash{}input\{evil\}\ \%\ \$\ \#\ \&\ \_", latex)
        self.assertIn(r"\textasciicircum{}\ \textasciitilde{}\ \textless{}\ \textgreater{}", latex)
        self.assertIn(r"\textasciicircum{}\textasciicircum{}7b", latex)
        self.assertIn(r"\textbackslash{}end\{document\}", latex)
        self.assertEqual(latex.count(r"\end{document}"), 1)
        self.assertNotIn(r"\input{evil}", latex)
        self.assertNotIn("\n'next", latex)

    def test_custom_templates_with_bom_crlf_and_atomic_file_output(self):
        source = self.root / "macro program.i"
        source.write_bytes(b'\xef\xbb\xbfIR_PRINT("{{title}}");\r\n')
        template = self.root / "my template"
        output = self.root / "my document"
        for target, skeleton, title_marker in (
            ("latex", "% literal {{other}}\r\n\\begin{document}\r\n"
             "\\section*{{{title}}}\r\n {{body}}\r\n\\end{document}",
             r"\section*{a\_b \{\{body\}\}}"),
            ("troff", '.\\" literal {{other}}\r\n{{title}}\r\n\t{{body}}\r\n.sp',
             r"\&a_b {{body}}"),
        ):
            with self.subTest(target=target):
                template.write_bytes(b"\xef\xbb\xbf" + skeleton.encode("utf-8"))
                output.write_text("old result", encoding="utf-8")
                actual = self.run_tool(
                    "--format", target, "--template", template, "--title", "a_b {{body}}",
                    "--out", output, source,
                )
                self.assertEqual(actual.stdout, "")
                document = output.read_text(encoding="utf-8")
                self.assertIn("{{other}}", document)
                self.assertIn(title_marker, document)
                self.assertIn("IR" + (r"\_" if target == "latex" else "_") + "PRINT(", document)
                self.assertNotIn("old result", document)
                self.assertNotIn(b"\r", output.read_bytes())
                self.assertTrue(output.read_bytes().endswith(b"\n"))
                self.assertEqual(list(self.root.glob(".PrettyPrint-*")), [])

    def test_invalid_input_or_template_does_not_replace_output(self):
        source = self.root / "Program.i"
        output = self.root / "report.tex"
        template = self.root / "template.tex"
        output.write_text("keep this", encoding="utf-8")
        for bad, diagnostic in (
            (b"\xff", "not valid UTF-8"),
            (b"\nIR_WORD([1)", "mismatched argument delimiter"),
            (b"\nIR_WORD(1", "unterminated call"),
            (b'IR_WORD("x\n");', "unterminated quoted token"),
            (b"/* beginning\n", "unterminated block comment"),
            (b"IR_WORD(" + b"(" * 128 + b"0", "too many nested argument delimiters"),
        ):
            with self.subTest(input=bad):
                source.write_bytes(bad)
                result = self.run_tool("--out", output, source, ok=False)
                self.assertIn(diagnostic, result.stderr)
                self.assertEqual(output.read_text(encoding="utf-8"), "keep this")
        source.write_bytes(b"IR_RETURN_VOID()")
        for bad in ("no body", "inline {{body}} here", "{{body}}\n{{body}}"):
            with self.subTest(template=bad):
                template.write_text(bad, encoding="utf-8")
                result = self.run_tool("--template", template, "--out", output, source, ok=False)
                self.assertIn("exactly one {{body}} on its own line", result.stderr)
                self.assertEqual(output.read_text(encoding="utf-8"), "keep this")
        result = self.run_tool("--out", source, source, ok=False)
        self.assertIn("must not overwrite input or template", result.stderr)
        self.assertEqual(source.read_bytes(), b"IR_RETURN_VOID()")

    def test_format_and_input_size_validation(self):
        result = self.run_tool("--format", "html", "-", source="", ok=False)
        self.assertIn("invalid choice", result.stderr)
        source = self.root / "large.i"
        with source.open("wb") as stream:
            stream.truncate(8 * 1024 * 1024 + 1)
        result = self.run_tool(source, ok=False)
        self.assertIn("input exceeds", result.stderr)


if __name__ == "__main__":
    unittest.main()
