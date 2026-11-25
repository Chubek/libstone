#!/usr/bin/env python3
"""Exercise named targets and the actual stage selections of Cephyr profiles."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import textwrap
import unittest


class ProfileTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="cephyr-profiles-")
        self.addCleanup(scratch.cleanup)
        self.work = Path(scratch.name)
        self.env = dict(os.environ, XDG_CACHE_HOME=str(self.work / "cache"), LC_ALL="C")
        self.env.pop("CEPHYR_CONFIG", None)
        self.write("input.c", """
            unsigned long identity(unsigned long x) { return (x + 0UL) * 1UL; }
            unsigned long literals(void) { return 6UL * 7UL; }
            unsigned long doubling(unsigned long x) { return x * 2UL; }
            unsigned long zext(void) { return 0xffffffffUL; }
            unsigned long sext(void) { return 0xffffffff80000000UL; }
            int boolean(unsigned long x) { if (x & 1UL) return 2; return 3; }
        """)

    def write(self, name, text):
        path = self.work / name
        path.write_text(textwrap.dedent(text).lstrip("\n"), encoding="utf-8")
        return path

    def cli(self, *args, ok=True):
        result = subprocess.run(
            [ARGS.cephyr, *args, "input.c"], cwd=self.work, env=self.env,
            capture_output=True, text=True, timeout=30,
        )
        self.assertEqual(result.returncode == 0, ok, result.stderr or result.stdout)
        if not ok:
            self.assertEqual(result.stdout, "")
        return result

    def mir(self, *args):
        return self.cli("--emit-mir", "-o", "-", *args).stdout

    def body(self, mir, name):
        return mir.split(f"\nfunction {name} ", 1)[1].split("\nfunction ", 1)[0]

    def test_target_precedence_and_legacy_selector_is_qml_only(self):
        config = """
            target = "x86_64-linux-gnu"
            [qml]
            profile = "cross"
            [profiles.cross]
            target = "aarch64-linux-gnu"
            [profiles.cross.factir]
            enabled = false
            [profiles.rv64]
            target = "riscv64-linux-gnu"
        """
        self.write("Cephyr.toml", config)

        def interface(target, *args):
            report = self.cli("--emit-interface=sexp", *args).stdout
            self.assertIn(f'(target "{target}")', report)
            return report

        inherited = interface("x86_64-linux-gnu")
        self.assertIn("(status analyzed)", inherited)
        self.write("Cephyr.toml", 'profile = "cross"\n' + config)
        self.assertIn("(status disabled)", interface("aarch64-linux-gnu"))
        self.assertIn("(status analyzed)", interface("riscv64-linux-gnu", "--profile=rv64"))
        self.assertIn("(status disabled)", interface("x86_64-linux-gnu", "--target=x86_64-linux-gnu"))
        self.cli("-c", "-o", "target.o")
        elf = (self.work / "target.o").read_bytes()
        self.assertTrue(elf.startswith(b"\x7fELF"))
        self.assertEqual(int.from_bytes(elf[18:20], "little"), 183)  # EM_AARCH64
        self.write("input.c", """
            #if defined(__aarch64__)
            int selected_arm;
            #else
            int selected_other;
            #endif
        """)
        self.assertIn("selected_arm", self.cli("-E", "-P").stdout)
        missing = self.cli("-E", "--profile=missing", ok=False)
        self.assertIn("unknown compilation profile 'missing'", missing.stderr)

    def test_graph_pass_exact_selection_and_cli_override(self):
        self.write("Cephyr.toml", """
            profile = "just-add"
            [mir]
            passes = ["mul-one"]
            [factir]
            enabled = false
            [blackstone]
            enabled = false
            [profiles.just-add]
            passes = ["strength_reduce"]
            [profiles.just-add.mir]
            passes = ["add-zero"]
            [profiles.inherit]
            [profiles.empty.mir]
            passes = []
        """)

        def identity_ops(*args):
            return re.findall(r"(?m)^(add|mul) ", self.body(self.mir(*args), "identity"))

        self.assertEqual(identity_ops(), ["mul"])
        self.assertEqual(identity_ops("--profile=inherit"), ["add"])
        self.assertEqual(identity_ops("--profile=empty"), ["add", "mul"])
        self.assertEqual(identity_ops("--no-mir-opt"), ["add", "mul"])
        self.assertEqual(identity_ops("--mir-opt", "--no-mir-opt", "--mir-opt"), [])
        doubling = self.body(self.mir(), "doubling")
        self.assertNotRegex(doubling, r"(?m)^mul ")
        self.assertRegex(doubling, r"(?m)^add \d+ (\d+) \1$")
        self.assertRegex(self.body(self.mir("--profile=inherit"), "doubling"), r"(?m)^mul ")

    def test_blackstone_matches_only_selected_families(self):
        self.write("Cephyr.toml", """
            [factir]
            enabled = false
            [profiles.fold.blackstone]
            passes = ["constant-fold"]
            [profiles.identities.blackstone]
            passes = ["identities"]
        """)
        fold = self.mir("--profile=fold")
        identities = self.mir("--profile=identities")
        off = self.mir("--profile=fold", "--no-blackstone")
        all_passes = self.mir("--profile=fold", "--blackstone")
        for ir in (fold, off):
            self.assertRegex(self.body(ir, "identity"), r"(?m)^add ")
        for ir in (identities, all_passes):
            self.assertNotRegex(self.body(ir, "identity"), r"(?m)^(add|mul) ")
        for ir in (identities, off):
            self.assertRegex(self.body(ir, "literals"), r"(?m)^mul ")
        for ir in (fold, all_passes):
            self.assertNotRegex(self.body(ir, "literals"), r"(?m)^mul ")

    def test_factir_analysis_and_individual_proofs(self):
        self.write("Cephyr.toml", """
            [blackstone]
            enabled = false
            [profiles.analysis.factir]
            passes = []
            [profiles.add-only.factir]
            passes = ["add-zero"]
            [profiles.bool-only.factir]
            passes = ["boolize"]
        """)
        base = self.mir("--no-factir")
        analysis = self.mir("--profile=analysis", "--factir-facts=facts", "--factir-smtlib=analysis.smt2")
        self.assertEqual(analysis, base)
        self.assertIn("[known_bits_i64]", (self.work / "facts").read_text())
        self.assertIn("rewrite passes disabled", (self.work / "analysis.smt2").read_text())
        for name, verify, skip in (("add-only", "bvadd", "extract"), ("bool-only", "extract", "bvadd")):
            with self.subTest(name=name):
                result = self.mir(f"--profile={name}", f"--factir-smtlib={name}.smt2")
                self.assertNotEqual(result, base)
                obligation = (self.work / f"{name}.smt2").read_text()
                self.assertIn("Satie: unsat", obligation)
                self.assertIn(verify, obligation)
                self.assertNotIn(skip, obligation)
        report = self.cli("--profile=analysis", "--emit-interface=sexp").stdout
        self.assertIn("(status analyzed)", report)
        self.assertIn("(rewrites 0)", report)
        self.assertEqual(self.mir("--profile=analysis", "--factir"), self.mir())

    def test_sandslab_forms_and_backend_compatibility(self):
        self.write("Cephyr.toml", """
            [profiles.short.sandslab]
            passes = ["sign-extend-imm32"]
        """)

        def assembly(*args, ok=True):
            return self.cli("-S", "-o", "-", "--profile=short", *args, ok=ok)

        selected = assembly().stdout
        self.assertIn("\tmovabsq $0xffffffff, %rax\n", selected)
        self.assertIn("\t.byte 72,199,192,0,0,0,128\n", selected)
        self.assertIn("\t.byte 184,255,255,255,255\n", assembly("--sandslab").stdout)
        self.assertIn("\tmovabsq $0xffffffff80000000, %rax\n", assembly("--no-sandslab").stdout)
        wrong = assembly("--target=aarch64-linux-gnu", ok=False)
        self.assertIn("require the native AMD64 backend", wrong.stderr)
        assembly("--target=aarch64-linux-gnu", "--no-sandslab")
        self.mir("--profile=short", "--target=aarch64-linux-gnu")

    def test_moonstone_availability_and_explicit_override(self):
        self.write("input.c", "int f(void) { int x; return x; }\n")
        self.write("Cephyr.toml", """
            [profiles.audit.moonstone]
            enabled = true
        """)
        args = ("-fsyntax-only", "--profile=audit")
        result = self.cli(*args, ok=ARGS.enabled)
        expected = ("cephyr.moonstone.chisel.uninitialized" if ARGS.enabled else
                    "built without Moonstone analysis support")
        self.assertIn(expected, result.stderr)
        self.assertNotIn("cephyr.moonstone.", self.cli(*args, "--no-moonstone-analyze").stderr)

    def test_strict_schema_for_unselected_profiles(self):
        for text, diagnostic in (
            ('profile = "missing"\n', "unknown compilation profile"),
            ('[profiles.wrong]\ntarget = "../amd64"\n', "target must be a nonempty target-bank name"),
            ('[profiles.wrong]\nmir = false\n', "profiles.wrong.mir' must be a table"),
            ('[profiles.wrong.moonstone]\nenabled = "yes"\n', "moonstone.enabled' must be a boolean"),
            ('[profiles.wrong.factir]\nextra = true\n', "profiles.wrong.factir.extra"),
            ('[profiles.wrong.mir]\npasses = ["verify-graph"]\n', "unknown mir pass 'verify-graph'"),
            ('[profiles.wrong.blackstone]\npasses = ["identities", "identities"]\n', "duplicate blackstone pass"),
            ('[profiles.wrong.sandslab]\npasses = [1]\n', "must contain only strings"),
            ('[profiles.wrong.factir]\nenabled = false\npasses = ["add-zero"]\n', "cannot select passes"),
        ):
            with self.subTest(text=text):
                self.write("Cephyr.toml", text)
                error = self.cli("-fsyntax-only", ok=False).stderr
                self.assertIn("Cephyr.toml:", error)
                self.assertIn(diagnostic, error)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("cephyr", type=lambda value: str(Path(value).resolve()))
    parser.add_argument("--enabled", action="store_true", help="the binary has Moonstone analysis")
    ARGS, extra = parser.parse_known_args()
    unittest.main(argv=[__file__, *extra])
