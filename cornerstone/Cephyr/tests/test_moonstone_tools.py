#!/usr/bin/env python3
"""Exercise the optional links through Cephyr, with no tool subprocess fallback."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


MOONSTONE = Path(__file__).resolve().parents[3] / "moonstone"
COMPONENTS = ("prolific", "plaid", "exoml", "exotract", "qmr")


class MoonstoneTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="cephyr-moonstone-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.env = os.environ.copy()
        self.env.pop("CEPHYR_CONFIG", None)
        self.env["LC_ALL"] = "C"
        self.env["XDG_CACHE_HOME"] = str(self.work / "cache")
        if OPTIONS.only_profile:
            self.env.pop("LD_LIBRARY_PATH", None)

    def file(self, name, data):
        path = self.work / name
        path.write_text(data, encoding="utf-8")
        return path

    def run_cephyr(self, *args, success=True, tool=False):
        env = self.env.copy()
        if tool:
            # These bridges must call linked libraries, not discover tools in PATH.
            env["PATH"] = str(self.work / "no-executables")
        result = self.run_command(OPTIONS.cephyr, *args, env=env)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertFalse(result.stdout, result.stdout)
        return result

    def run_command(self, *args, env=None):
        return subprocess.run(
            args, cwd=self.work, env=env or self.env, capture_output=True,
            text=True, check=False, timeout=30,
        )

    def test_profile(self):
        self.file("ok.c", "int main(void) { return 0; }\n")
        self.file("broken.c", "int broken( { }\n")
        if "prolific" not in OPTIONS.have:
            result = self.run_cephyr(
                "--moonstone-profile=declined.json", "-fsyntax-only", "ok.c", success=False,
            )
            self.assertIn("prolific", result.stderr.lower())
            self.assertFalse((self.work / "declined.json").exists())
            return

        for ok, source in ((True, "ok.c"), (False, "broken.c")):
            with self.subTest(source=source):
                filename = f"{source}.json"
                result = self.run_cephyr(
                    f"--moonstone-profile={filename}", "--target=x86_64-linux-gnu",
                    "-fsyntax-only", source, success=ok,
                )
                self.assertEqual(bool(result.stderr), not ok, result.stderr)
                profile = json.loads((self.work / filename).read_text(encoding="utf-8"))
                self.assertEqual(profile["schema"], "moonstone.prolific/1")
                self.assertTrue(profile["complete"], profile)
                self.assertEqual(profile["rejected"], 0)
                self.assertEqual(profile["open_spans"], 0)
                self.assertEqual(profile["analysis"]["status"], "not_requested")
                events = profile["events"]
                self.assertTrue(events)
                spans = {}
                for event in events:
                    self.assertEqual(event["domain"], "compiler")
                    if event["kind"] == "begin":
                        spans[event["span"]] = event
                    if event["kind"] == "end":
                        begin = spans.pop(event["span"])
                        self.assertEqual(begin["name"], event["name"])
                        self.assertEqual(begin["thread"], event["thread"])
                        self.assertGreaterEqual(event["duration_ns"], event["self_ns"])
                self.assertFalse(spans)
                begins = [event for event in events if event["kind"] == "begin"
                          and event["location"]["file"] == source]
                self.assertTrue(any("preprocess" in event["name"] for event in begins), events)
                self.assertTrue(any("parse" in event["name"] for event in begins), events)
                self.assertTrue(all(event["location"]["line"] == 0 for event in begins))
                outcome = [event for event in events if event["name"] == "cephyr.result"]
                self.assertEqual(len(outcome), 1)
                self.assertEqual(outcome[0]["kind"], "instant" if ok else "error")
                self.assertEqual(outcome[0]["detail"], "success" if ok else "failure")

        stdout_result = self.run_cephyr(
            "--moonstone-profile=-", "-fsyntax-only", "ok.c", success=False,
        )
        self.assertIn("not standard output", stdout_result.stderr)
        collision = self.run_cephyr(
            "--moonstone-profile=ok.c", "-fsyntax-only", "ok.c", success=False,
        )
        self.assertIn("overwrite", collision.stderr)
        self.assertEqual((self.work / "ok.c").read_text(), "int main(void) { return 0; }\n")
        contents = "# left intact; collision is rejected before reading config\n"
        config = self.file("cephyr.toml", contents)
        collision = self.run_cephyr(
            "--moonstone-profile=cephyr.toml", "--config=cephyr.toml",
            "-fsyntax-only", "ok.c", success=False,
        )
        self.assertIn("overwrite the configuration file", collision.stderr)
        self.assertEqual(config.read_text(), contents)
        self.file("ok.s", "existing implicit output")
        collision = self.run_cephyr(
            "--moonstone-profile=ok.s", "-S", "ok.c", success=False,
        )
        self.assertIn("different files", collision.stderr)
        self.assertEqual((self.work / "ok.s").read_text(), "existing implicit output")
        self.file("collision.s", "existing explicit output")
        collision = self.run_cephyr(
            "--moonstone-profile=collision.s", "-S", "-o", "collision.s", "ok.c", success=False,
        )
        self.assertIn("different files", collision.stderr)
        self.assertEqual((self.work / "collision.s").read_text(), "existing explicit output")

        with_profile = (
            "--moonstone-profile=mir-profile.json", "--target=x86_64-linux-gnu",
            "--emit-mir", "-o", "profiled.mir", "ok.c",
        )
        self.run_cephyr(*with_profile)
        self.run_cephyr("--target=x86_64-linux-gnu", "--emit-mir", "-o", "plain.mir", "ok.c")
        self.assertEqual(
            (self.work / "profiled.mir").read_bytes(), (self.work / "plain.mir").read_bytes(),
        )
        profile = json.loads((self.work / "mir-profile.json").read_text(encoding="utf-8"))
        self.assertIn("lower", {event["name"] for event in profile["events"]})

        # A Linux pathname need not be UTF-8. Prolific rejects the observations
        # carrying this name without changing a successful compiler outcome.
        invalid_utf8_name = os.fsdecode(b"opaque-\xff.c")
        self.file(invalid_utf8_name, "int named(void) { return 1; }\n")
        self.run_cephyr(
            "--moonstone-profile=loss.json", "--target=x86_64-linux-gnu",
            "-fsyntax-only", invalid_utf8_name,
        )
        profile = json.loads((self.work / "loss.json").read_text(encoding="utf-8"))
        self.assertFalse(profile["complete"])
        self.assertGreater(profile["rejected"], 0)
        self.assertTrue(any(event["name"] == "cephyr.result" and event["detail"] == "success"
                            for event in profile["events"]))

    def test_exoml(self):
        command = "--moonstone-exoml"
        if "exoml" not in OPTIONS.have:
            result = self.run_cephyr(command, "input.exo", "Source", success=False, tool=True)
            self.assertIn("without Moonstone ExoML", result.stderr)
            return
        script = MOONSTONE / "exoml" / "examples" / "fixture.exo"
        output = self.run_cephyr(command, script, "Source", tool=True).stdout
        self.assertIn("bind_Script", output)
        self.assertIn("exoml_add", output)
        generated = self.file("bindings.hpp", output)
        result = self.run_command(
            OPTIONS.cxx, "-std=c++20", "-fsyntax-only", "-x", "c++",
            "-I", MOONSTONE / "exoml" / "include", "-I", MOONSTONE / "exoml" / "tests",
            generated,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        fixture = (MOONSTONE / "exoml" / "tests" / "fixture.h").read_text(encoding="utf-8")
        self.file("fixture.h", fixture.replace("int exoml_add(", "double exoml_add("))
        result = self.run_command(
            OPTIONS.cxx, "-std=c++20", "-fsyntax-only", "-x", "c++",
            "-I", MOONSTONE / "exoml" / "include", generated,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("static assertion", result.stderr.lower())
        result = self.run_cephyr(command, script, "Missing", success=False, tool=True)
        self.assertIn("must name a string symbol", result.stderr)
        hatch = self.file("hatch.exo", 'M ~hatch! StaticFFI:Module("Disabled");\n')
        result = self.run_cephyr(command, hatch, "M", success=False, tool=True)
        self.assertIn("hatch commands are not permitted", result.stderr)
        self.assertIn("hatch.exo:1:", result.stderr)

    def test_exotract(self):
        command = "--moonstone-exotract"
        if "exotract" not in OPTIONS.have:
            result = self.run_cephyr(command, "fixture.h", "FIXTURE_IMPL", success=False, tool=True)
            self.assertIn("without Moonstone Exotract", result.stderr)
            return
        header = self.file(
            "fixture.h", "int fixture(void);\n"
            "#ifdef FIXTURE_IMPL\nint fixture(void) { return 42; }\n#endif\n",
        )
        result = self.run_cephyr(command, header, "FIXTURE_IMPL", tool=True)
        generated = self.file("fixture.c", result.stdout)
        self.run_cephyr("--target=x86_64-linux-gnu", "-fsyntax-only", generated)
        self.file("main.c", '#include "fixture.h"\nint main(void) { return fixture() != 42; }\n')
        result = self.run_command(OPTIONS.cc, "-std=c11", generated, "main.c", "-o", "fixture")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_command("./fixture")
        self.assertEqual(result.returncode, 0, result.stderr)
        for macros in (("FIXTURE_IMPL", "FIXTURE_IMPL"), ("INJECT=1",)):
            result = self.run_cephyr(command, header, *macros, success=False, tool=True)
            self.assertIn("invalid or duplicate implementation macro", result.stderr)

    def test_qmr_and_plaid(self):
        compile_command = "--moonstone-qmr-compile"
        check_command = "--moonstone-qmr-check"
        if "qmr" not in OPTIONS.have:
            for command in (compile_command, check_command):
                result = self.run_cephyr(command, "item.qmr", success=False, tool=True)
                self.assertIn("without Moonstone QMR", result.stderr)
            return
        # A valid template containing a failing runtime action: compile/check
        # must output or translate it without executing any Lua.
        template = self.file("item.qmr", "Hello {{ data.name }}!\n{% error('do not execute') %}\n")
        result = self.run_cephyr(compile_command, template, tool=True)
        self.assertIn("__qmr_emit((data.name), true)", result.stdout)
        self.assertIn("error('do not execute')", result.stdout)
        self.run_cephyr(check_command, template, tool=True)
        files = [self.file(f"item-{i}.qmr", f"#{i} {{% include 'not-resolved' %}}\n")
                 for i in range(19)]
        sequential = self.run_cephyr(check_command, *files, tool=True)
        self.assertEqual(sequential.stdout, "19 QMR template(s) translated\n")
        result = self.run_cephyr(check_command, "--jobs=0", template, success=False, tool=True)
        self.assertIn("--jobs expects an integer from 1 to 8", result.stderr)
        if "plaid" not in OPTIONS.have:
            result = self.run_cephyr(check_command, "--jobs=2", *files, success=False, tool=True)
            self.assertIn("without Moonstone Plaid", result.stderr)
            return
        for jobs in (2, 3, 8, 2, 8):
            result = self.run_cephyr(check_command, f"--jobs={jobs}", *files, tool=True)
            self.assertEqual(result.stdout, sequential.stdout)

        broken = self.file("broken.qmr", "first line\n{{ data.name")
        bad_second = self.file("empty.qmr", "second file\n{{ }}")
        mixed = [*files[:6], broken, "not-found.qmr", *files[6:], bad_second]
        sequential = self.run_cephyr(check_command, *mixed, success=False, tool=True)
        self.assertIn("broken.qmr:2:1:", sequential.stderr)
        self.assertIn("QMR_UNCLOSED", sequential.stderr)
        self.assertIn("QMR_EMPTY", sequential.stderr)
        self.assertLess(sequential.stderr.index("QMR_UNCLOSED"), sequential.stderr.index("not-found.qmr"))
        self.assertLess(sequential.stderr.index("not-found.qmr"), sequential.stderr.index("QMR_EMPTY"))
        for jobs in (2, 4, 8):
            result = self.run_cephyr(check_command, f"--jobs={jobs}", *mixed, success=False, tool=True)
            self.assertEqual(result.stderr, sequential.stderr)
        oversize = self.file("oversize.qmr", " " * (1024 * 1024 + 1))
        result = self.run_cephyr(check_command, "--jobs=2", template, oversize,
                                 success=False, tool=True)
        self.assertIn("input exceeds byte limit", result.stderr)
        fifo = self.work / "fifo.qmr"
        os.mkfifo(fifo)
        result = self.run_cephyr(check_command, "--jobs=2", template, fifo,
                                 success=False, tool=True)
        self.assertIn("expected a regular input file", result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=lambda path: Path(path).resolve())
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--have", action="append", choices=COMPONENTS, default=[])
    parser.add_argument("--only-profile", action="store_true")
    OPTIONS = parser.parse_args()
    unittest.main(argv=[__file__] + (["MoonstoneTests.test_profile"] if OPTIONS.only_profile else []))
