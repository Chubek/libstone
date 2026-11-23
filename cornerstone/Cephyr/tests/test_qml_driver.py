#!/usr/bin/env python3
"""Exercise QML through the CLI, including profile and runtime asset discovery."""
import argparse
import json
import os
from pathlib import Path
import platform
import random
import re
import subprocess
import tempfile
import unittest


SOURCE = "unsigned long transform(unsigned long x) { return ((x + 0ul) - 0ul) + 0ul; }\n"
ADD = "rewrite local_add remove_add_zero : add(?x, 0) => ?x;\n"
SUB = "rewrite local_sub remove_sub_zero : sub(?x, 0) => ?x;\n"
DIFFERENTIAL = Path(__file__).with_name("fixtures") / "qml_differential"


def differential_inputs():
    def signed_boundaries(bits):
        half = 1 << (bits - 1)
        return (-half, -half + 1, -1, 0, 1, half - 2, half - 1)

    def unsigned_boundaries(bits):
        half = 1 << (bits - 1)
        return (0, 1, half - 1, half, half + 1, 2 * half - 2, 2 * half - 1)

    yield from zip(signed_boundaries(64), signed_boundaries(32),
                   unsigned_boundaries(64), unsigned_boundaries(32))
    rng = random.Random(0x514D4C)
    for _ in range(24):
        yield (rng.getrandbits(64) - (1 << 63), rng.getrandbits(32) - (1 << 31),
               rng.getrandbits(64), rng.getrandbits(32))


def differential_expected(inputs):
    mask64 = (1 << 64) - 1
    mask32 = (1 << 32) - 1
    for _, _, u64, u32 in inputs:
        seen = (2 * u64) & mask64
        cell = (seen >> 1) ^ u32
        returned = (2 * (seen + u32) + cell + u64 % (1 << 63)) & mask64
        yield (2 * u32) & mask32, returned, cell, seen


def differential_initializer(case, expected):
    def signed_literal(value, suffix):
        # Even signed minima must not rely on unsigned-to-signed conversion.
        return f"(-{-value - 1}{suffix} - 1{suffix})" if value < 0 else f"{value}{suffix}"

    s64, s32, u64, u32 = case
    doubled32, returned, cell, seen = expected
    return (f"{{{signed_literal(s64, 'L')}, {signed_literal(s32, '')}, {u64}UL, {u32}U, "
            f"{doubled32}U, {returned}UL, {cell}UL, {seen}UL}},\n")


class QmlDriverTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="cephyr-qml-driver-")
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)
        self.env = os.environ.copy()
        self.env.pop("CEPHYR_CONFIG", None)
        self.env["XDG_CACHE_HOME"] = str(self.work / "cache")
        # The baseline and profile comparisons measure only QML rewrites.
        self.file("Cephyr.toml", "[blackstone]\nenabled = false\n")
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
            [ARGS.cephyr, "--no-factir", *args], cwd=self.work, env=env, capture_output=True,
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

    def test_guarded_integer_reductions(self):
        self.file("input.c", """
volatile unsigned long sample;
unsigned long reduce(void) { return sample % (1UL << 8); }
unsigned long masked(unsigned long x) { return (x & 255UL) & 65535UL; }
unsigned long partial_mask(unsigned long x, unsigned long y) {
    return (y | 255UL) & (x & 255UL);
}
unsigned long narrow_mask(unsigned long x) { return (x & 255UL) & 127UL; }
unsigned long odd_divisor(unsigned long x) { return x % 255UL; }
unsigned long unknown_divisor(unsigned long x, unsigned long y) { return x % y; }
unsigned long maybe_zero_divisor(unsigned long x, unsigned long y) {
    return x % (y & 256UL);
}
long signed_remainder(long x) { return x % 256L; }
double floating(double x) { return x * 256.0; }
""")
        baseline = self.mir(no_tools=True).stdout
        selected = self.mir("--qml-pass=integer_reduce", no_tools=True).stdout

        def function(mir, name):
            return mir.split(f"\nfunction {name} ", 1)[1].split("\nfunction ", 1)[0]

        before = function(baseline, "reduce")
        after = function(selected, "reduce")
        self.assertRegex(before, r"(?m)^rem_u ")
        self.assertNotRegex(after, r"(?m)^rem_u ")
        self.assertRegex(after, r"(?m)^trunc \d+ \d+ 8$")
        self.assertEqual(re.findall(r"(?m)^load .*$", after),
                         re.findall(r"(?m)^load .*$", before))
        self.assertEqual(len(re.findall(r"(?m)^load ", after)), 1)
        self.assertEqual(len(re.findall(r"(?m)^and ", function(baseline, "masked"))), 2)
        self.assertEqual(len(re.findall(r"(?m)^and ", function(selected, "masked"))), 1)
        self.assertEqual(len(re.findall(r"(?m)^and ", function(baseline, "partial_mask"))), 2)
        self.assertEqual(len(re.findall(r"(?m)^and ", function(selected, "partial_mask"))), 1)
        for name in ("narrow_mask", "odd_divisor", "unknown_divisor",
                     "maybe_zero_divisor", "signed_remainder", "floating"):
            with self.subTest(name=name):
                self.assertEqual(function(selected, name), function(baseline, name))

    def test_rewrites_preserve_wrapping_values_and_call_count(self):
        self.file("input.c", """
unsigned int count;
unsigned long next_value(void) {
    ++count;
    return 0xffffffffffffffffUL;
}
volatile unsigned long cell;
int main(void) {
    unsigned long y = ((next_value() + 0UL) * 2UL) ^ 0UL;
    cell = y;
    unsigned long remainder = cell % (1UL << 63);
    unsigned long masked = (cell & 255UL) & 65535UL;
    return y != 0xfffffffffffffffeUL || count != 1U ||
           remainder != 0x7ffffffffffffffeUL || masked != 254UL;
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
                                "--qml-pass=strength_reduce", "--qml-pass=integer_reduce",
                                "input.c", "-o", triple)
                if host in native_hosts or emulator:
                    runner = [] if host in native_hosts else [emulator]
                    result = subprocess.run([*runner, str(self.work / triple)],
                                            capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result)

    def test_native_pipelines_against_boundary_and_seeded_reference(self):
        inputs = list(differential_inputs())
        expected = list(differential_expected(inputs))
        fields = ("signed64", "signed32", "unsigned32", "unsigned64 result",
                  "cell after call", "cell seen by call", "call count")
        self.assertLessEqual(len(inputs) * len(fields), 255)
        self.file("inputs.inc", "".join(
            differential_initializer(case, result) for case, result in zip(inputs, expected)
        ))
        for name in ("kernel.c", "driver.c", "start.S"):
            self.file(name, (DIFFERENTIAL / name).read_text(encoding="utf-8"))

        host = platform.machine().lower()
        targets = (
            ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
            ("aarch64-linux-gnu", {"aarch64", "arm64"}, ARGS.qemu_aarch64),
            ("riscv64-linux-gnu", {"riscv64"}, ARGS.qemu),
        )
        pipelines = {
            "baseline": (),
            "canonicalize": ("canonicalize",),
            "strength_reduce": ("strength_reduce",),
            "combined": ("canonicalize", "strength_reduce"),
            "integer_reduce": ("integer_reduce",),
            "all": ("canonicalize", "strength_reduce", "integer_reduce"),
        }
        for triple, native_hosts, emulator in targets:
            target = f"--target={triple}"
            self.run_cephyr(target, "-c", "driver.c", "-o", "driver.o")
            self.run_cephyr(target, "-c", "start.S", "-o", "start.o")
            for name, passes in pipelines.items():
                with self.subTest(target=triple, pipeline=name):
                    flags = tuple(f"--qml-pass={phase}" for phase in passes)
                    mir = self.run_cephyr(target, "--emit-mir", "-o", "-", "kernel.c", *flags).stdout
                    # Check each selected pass really did work, without pinning
                    # the total number of rewrite opportunities or SSA IDs.
                    if "canonicalize" in passes:
                        self.assertNotRegex(mir, r"(?m)^xor ")
                    else:
                        self.assertRegex(mir, r"(?m)^xor ")
                    if "strength_reduce" in passes:
                        self.assertRegex(mir, r"(?m)^add \d+ (\d+) \1$")
                    else:
                        self.assertNotRegex(mir, r"(?m)^add \d+ (\d+) \1$")
                    if "integer_reduce" in passes:
                        self.assertNotRegex(mir, r"(?m)^rem_u ")
                    else:
                        self.assertRegex(mir, r"(?m)^rem_u ")

                    binary = self.work / "differential"
                    self.run_cephyr(target, "-nostdlib", "-static", "kernel.c",
                                    "driver.o", "start.o", *flags, "-o", str(binary))
                    if host not in native_hosts and emulator is None:
                        continue
                    runner = [] if host in native_hosts else [emulator]
                    result = subprocess.run([*runner, str(binary)], cwd=self.work,
                                            capture_output=True, text=True, timeout=20, check=False)
                    if 0 < result.returncode <= len(inputs) * len(fields):
                        case, field = divmod(result.returncode - 1, len(fields))
                        self.fail(f"{fields[field]} mismatch for case {case}: {inputs[case]}; "
                                  f"expected unsigned observations: {expected[case]}")
                    self.assertEqual(result.returncode, 0, result)

    def test_profile_file_paths_merge_and_refresh(self):
        profile = self.file("cfg/Cephyr.toml", """
[blackstone]
enabled = false

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
        self.assertIn("loading packaged Archon backend", first.stderr)
        self.file("rules.qpass", rule.format(8))
        second = self.run_cephyr(*args)
        # Packaged backends bypass the generated-source cache. Both invocations
        # should reuse the same provider but still emit the edited QML rule.
        package = re.search(r"loading packaged Archon backend [^\n]+", first.stderr)
        if package:
            self.assertIn(package[0], second.stderr)
        else:
            self.assertIn("loading packaged Archon backend", second.stderr)
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
