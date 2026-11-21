#!/usr/bin/env python3
"""Exercise QML through the CLI, including profile and runtime asset discovery."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import unittest


SOURCE = "unsigned long transform(unsigned long x) { return ((x + 0ul) - 0ul) + 0ul; }\n"
ADD = "rewrite local_add remove_add_zero : add(?x, 0) => ?x;\n"
SUB = "rewrite local_sub remove_sub_zero : sub(?x, 0) => ?x;\n"


class QmlDriverTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="cephyr-qml-driver-")
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)
        self.env = os.environ.copy()
        self.env.pop("CEPHYR_CONFIG", None)
        self.env["XDG_CACHE_HOME"] = str(self.work / "cache")
        self.file("input.c", SOURCE)

    def file(self, name, text):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def run_cephyr(self, *args, success=True, no_tools=False):
        env = self.env.copy()
        if no_tools:
            env["PATH"] = str(self.work / "no-programs")
        result = subprocess.run(
            [ARGS.cephyr, *args], cwd=self.work, env=env, capture_output=True,
            text=True, check=False, timeout=60,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertFalse(result.stdout, result.stdout)
        return result

    def mir(self, *args, **kwargs):
        return self.run_cephyr("--emit-mir", "-o", "-", "input.c", *args, **kwargs)

    def assert_instructions(self, mir, *, adds, subs):
        self.assertEqual(len(re.findall(r"^add ", mir, re.M)), adds, mir)
        self.assertEqual(len(re.findall(r"^sub ", mir, re.M)), subs, mir)

    def test_opt_in_builtin_does_not_require_python(self):
        self.assert_instructions(self.mir(no_tools=True).stdout, adds=2, subs=1)
        result = self.mir("--qml-pass=canonicalize", no_tools=True)
        self.assert_instructions(result.stdout, adds=0, subs=0)

    def test_builtin_strength_reduction_is_a_separate_pass(self):
        self.file("input.c", "unsigned long f(unsigned long x) { return (x * 2ul) ^ 0ul; }\n")
        identities = self.mir("--qml-pass=canonicalize", no_tools=True).stdout
        self.assertRegex(identities, r"(?m)^mul ")
        self.assertNotRegex(identities, r"(?m)^xor ")
        both = self.mir("--qml-pass=canonicalize", "--qml-pass=strength_reduce", no_tools=True).stdout
        self.assertNotRegex(both, r"(?m)^(mul|xor) ")
        self.assertRegex(both, r"(?m)^add \d+ (\d+) \1$")

    def test_rewrites_preserve_wrapping_values_and_call_count(self):
        self.file("input.c", """
unsigned int count;
unsigned long next_value(void) {
    ++count;
    return 0xffffffffffffffffUL;
}
int main(void) {
    unsigned long y = ((next_value() + 0UL) * 2UL) ^ 0UL;
    return y != 0xfffffffffffffffeUL || count != 1U;
}
""")
        host = platform.machine().lower()
        targets = (
            ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
            ("aarch64-linux-gnu", {"aarch64", "arm64"}, ARGS.qemu_aarch64),
            ("riscv64-linux-gnu", {"riscv64"}, ARGS.qemu),
        )
        for triple, native_hosts, emulator in targets:
            with self.subTest(target=triple):
                self.run_cephyr(f"--target={triple}", "--qml-pass=canonicalize",
                                "--qml-pass=strength_reduce", "input.c", "-o", triple)
                if host in native_hosts or emulator:
                    runner = [] if host in native_hosts else [emulator]
                    result = subprocess.run([*runner, str(self.work / triple)],
                                            capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result)

    def test_profile_file_paths_merge_and_refresh(self):
        profile = self.file("cfg/Cephyr.toml", """
[qml]
profile = "local"

[profiles.local]
passes = ["local_add"]
files = ["pass.qpass"]
fuel = 12
parameters = { local_add = { fuel = 2 } }

[profiles.plain]

[profiles.order]
passes = ["swap"]
files = ["swap.qpass"]
""")
        self.file("cfg/pass.qpass", ADD)
        self.file("cfg/swap.qpass", "rewrite swap sub_to_add : sub(?x, ?y) => add(?x, ?y);\n")
        sub_file = self.file("sub.qpass", SUB)
        common = ("--config", str(profile))
        self.assert_instructions(self.mir(*common).stdout, adds=0, subs=1)
        self.assert_instructions(self.mir(*common, "--profile=plain").stdout, adds=2, subs=1)
        custom = (*common, "--qml-pass", "local_sub", "--qml-file=sub.qpass")
        self.assert_instructions(self.mir(*custom).stdout, adds=0, subs=0)
        # The edit changes a rule without changing its path or size.
        sub_file.write_text(SUB.replace("sub(?x, 0)", "sub(?x, 1)"), encoding="utf-8")
        self.assert_instructions(self.mir(*custom).stdout, adds=0, subs=1)
        ordered = (*common, "--profile=order", "--qml-pass=local_add", "--qml-file=cfg/pass.qpass")
        self.assert_instructions(self.mir(*ordered).stdout, adds=0, subs=0)

    def test_fuel_and_diagnostics(self):
        profile = self.file("cfg/Cephyr.toml", """
[profiles.small]
passes = ["canonicalize"]
fuel = 16
parameters = { canonicalize = { fuel = 1 } }
""")
        common = ("--config", str(profile), "--profile=small")
        bad = self.mir(*common, success=False)
        self.assertIn("fuel exhausted", bad.stderr)
        self.assert_instructions(
            self.mir(*common, "--qml-param=canonicalize.fuel=3").stdout, adds=0, subs=0,
        )
        for opts, diagnostic in [
            (("--profile=missing",), "unknown QML profile"),
            (("--qml-pass=missing",), "phase"),
            (("--qml-file=absent.qpass", "--qml-pass=canonicalize"), "absent.qpass"),
            (("--qml-file=absent.qpass",), "select at least one"),
            (("--qml-fuel=-1",), "positive unsigned integer"),
            (("--qml-pass=canonicalize", "--qml-param=canonicalize.unknown=2"), "unknown QML parameter"),
            (("--qml-pass=canonicalize", "--qml-param=other.fuel=2"), "unselected QML pass"),
            (("--qml-pass=canonicalize", "--qml-fuel=1"), "fuel exhausted"),
        ]:
            with self.subTest(opts=opts):
                self.assertIn(diagnostic, self.mir(*opts, success=False).stderr)
        invalid_config = self.file("cfg/invalid.toml", """
[profiles.wrong]
parameters = { canonicalize = { unknown = 3 } }
""")
        self.assertIn("profiles.wrong.parameters.canonicalize.unknown",
                      self.mir("--config", str(invalid_config), success=False).stderr)

    def test_frontend_only_skips_qml_rules(self):
        self.run_cephyr("-E", "--qml-pass=unknown", "--qml-file=absent.qpass", "input.c")
        self.run_cephyr("-fsyntax-only", "--profile=unknown", "input.c")

    def test_bad_rule_leaves_no_mir_output(self):
        broken = self.file("broken.qpass", "rewrite local a : add(?x, 0) => ?unbound;\n")
        result = self.mir("--qml-pass=local", "--qml-file", str(broken), success=False)
        self.assertIn("broken.qpass", result.stderr)
        self.assertIn("Unbound", result.stderr)

    def test_native_output_after_rule_edit_with_warm_cache(self):
        self.file("input.c", SOURCE.replace("((x + 0ul) - 0ul) + 0ul", "x + 7ul"))
        rule = "rewrite local strip_add : add(?x, {}) => ?x;\n"
        self.file("rules.qpass", rule.format(7))
        args = ("-S", "-o", "-", "-v", "--cache", "--qml-pass=local",
                "--qml-file=rules.qpass", "input.c")
        first = self.run_cephyr(*args)
        self.assertNotIn("Archon IRGen cache hit", first.stderr)
        self.file("rules.qpass", rule.format(8))
        second = self.run_cephyr(*args)
        self.assertIn("Archon IRGen cache hit", second.stderr)
        self.assertNotEqual(first.stdout, second.stdout)

    def test_multifile_configuration_and_timing(self):
        if not ARGS.prolific:
            self.skipTest("Cephyr built without Prolific")
        self.file("second.c", SOURCE.replace("transform", "another"))
        self.file("measured.qpass", SUB)
        result = self.run_cephyr(
            "--moonstone-profile=report.json", "--emit-mir", "--qml-pass=local_sub",
            "--qml-file=measured.qpass", "-v", "input.c", "second.c",
        )
        self.assertEqual(result.stderr.count("compile_rules.py"), 1, result.stderr)
        report = json.loads((self.work / "report.json").read_text(encoding="utf-8"))
        begins = [event for event in report["events"] if event["kind"] == "begin"]
        self.assertEqual(sum(event["name"] == "qml_configure" for event in begins), 1)
        for name in ("qml_rewrite", "qml_verify"):
            files = [event["location"]["file"] for event in begins if event["name"] == name]
            self.assertEqual(files, ["input.c", "second.c"])
        for path in ("input.mir", "second.mir"):
            self.assert_instructions((self.work / path).read_text(encoding="utf-8"), adds=2, subs=0)

    def test_no_match_does_not_reverify_canonical_mir(self):
        if not ARGS.prolific:
            self.skipTest("Cephyr built without Prolific")
        self.file("input.c", "unsigned long f(unsigned long x) { return x + 7UL; }\n")
        baseline = self.mir().stdout
        selected = self.mir("--qml-pass=canonicalize", "--moonstone-profile=report.json")
        self.assertEqual(selected.stdout, baseline)
        events = json.loads((self.work / "report.json").read_text(encoding="utf-8"))["events"]
        phases = {event["name"] for event in events if event["kind"] == "end"}
        self.assertIn("qml_rewrite", phases)
        self.assertNotIn("qml_verify", phases)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=lambda path: str(Path(path).resolve()))
    parser.add_argument("--prolific", action="store_true")
    parser.add_argument("--qemu")
    parser.add_argument("--qemu-aarch64")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
