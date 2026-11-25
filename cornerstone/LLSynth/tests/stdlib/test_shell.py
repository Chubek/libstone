"""Test std.shell using the actual vendored LibShell command-line runtime."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


LLS = Path(__file__).resolve().parents[2]
DRIVER = LLS / "scripts" / "llsynth.pl"
SHELL = LLS / "stdlib" / "shell.llsynth"
BUILD = os.environ.get("LLSYNTH_SHELL_BUILD")
CC = shlex.split(os.environ.get("CC", "cc"))


def spec(action, import_line='.import "std.shell" as Sh'):
    return f'''.language "Shell integration"
{import_line}
.exception !Mismatch "unexpected shell result"
.rule Start
'syntax: EOF
'semantics: {action}
.end
'''


class ShellTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)

    def call(self, *args, ok=True, env=None):
        result = subprocess.run(args, cwd=self.work, input="", text=True,
                                capture_output=True, timeout=45, env=env)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def generate(self, text, *args, ok=True):
        path = self.work / "spec.llsynth"
        path.write_text(text, encoding="utf-8")
        return self.call("perl", DRIVER, "--mode=interpret", *args, path, ok=ok)

    def test_generation_and_module_types_without_native_dependency(self):
        self.generate(spec('let result: %Sh->Result = Sh:run("exit 0"); return result.Status;'),
                      "-o", "tool.py", "--depfile", "tool.d")
        self.assertIn(str(SHELL), (self.work / "tool.d").read_text())
        self.generate(spec('let result = std.shell:run(":"); return result.Status;',
                           ".import qualified std.shell"), "--check")
        for action, diagnostic in [("return Sh:run(42);", "type mismatch"),
                                   ("return Sh:run();", "expects 1 arguments"),
                                   ('return Sh:runPacket(":");', "does not export runPacket")]:
            with self.subTest(action=action):
                self.assertIn(diagnostic, self.generate(spec(action), "--check", ok=False).stderr)
        self.generate(spec("return 42;"), "-o", "lazy.py")
        self.assertEqual(self.call(sys.executable, "lazy.py").stdout, "42\n")

    def native_env(self):
        if not BUILD:
            self.skipTest("set LLSYNTH_SHELL_BUILD to the dedicated std.shell build directory")
        build = Path(BUILD).resolve()
        libdir = next((root for root in (build, build / "lib")
                       if (root / "libllsynth_shell.so").is_file()), None)
        libsh = next((root / "libsh" for root in (build / "libshell/cli", build / "bin")
                      if (root / "libsh").is_file()), None)
        self.assertIsNotNone(libdir, f"missing companion under {build}")
        self.assertIsNotNone(libsh, f"missing libsh under {build}")
        env = os.environ.copy()
        env.pop("LLSYNTH_LIBSH", None)
        env["PATH"] = os.pathsep.join((str(libsh.parent), env["PATH"]))
        for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
            env[key] = os.pathsep.join(filter(None, (str(libdir), env.get(key))))
        return libdir, env

    def tools(self, text, *, split=False, standard="c11"):
        build, env = self.native_env()
        self.generate(text, "-T", "python3", "-o", "tool.py")
        yield [sys.executable, "tool.py"], env
        output = self.work / "native"
        output.mkdir(exist_ok=True)
        self.generate(text, "-T", standard, "--split", "all" if split else "none",
                      "--out-dir", output, "-o", output / "tool.c")
        self.call(*CC, f"-std={standard}", "-pedantic-errors", *sorted(output.glob("*.c")),
                  "-L", build, "-lllsynth_shell", "-lm", "-o", output / "tool")
        yield [output / "tool"], env

    def test_script_features_and_exact_streams(self):
        script = """\
demo() { printf '%s:%s\\n' "$1" "$((2 + 3))"; }
NAME='a b'
if test "$NAME" = 'a b'; then
    for item in rose iris; do demo "$item"; done
fi
message=$(printf 'data\\n' | tr a-z A-Z)
printf 'msg=%s\\n' "$message"
cat <<'HERE'
literal $NAME
HERE
printf 'héllo\\n' > redirected.txt
cat < redirected.txt
echo warned >&2
if true; then exit 7; fi
echo unreachable
"""
        expected = "rose:5\niris:5\nmsg=DATA\nliteral $NAME\nhéllo\n"
        action = (
            f'let result: %Sh->Result = Sh:run({json.dumps(script, ensure_ascii=False)}); '
            f'require result.Status == 7 && result.Stdout == {json.dumps(expected, ensure_ascii=False)} '
            '&& result.Stderr == "warned\\n" else Throw(!Mismatch); return result.Status;'
        )
        for tool, env in self.tools(spec(action), split=True):
            with self.subTest(tool=tool):
                redirected = self.work / "redirected.txt"
                self.assertFalse(redirected.exists())
                result = self.call(*tool, env=env)
                self.assertEqual((result.stdout, result.stderr), ("7\n", ""))
                self.assertEqual(redirected.read_text(encoding="utf-8"), "héllo\n")
                redirected.unlink()

    def test_calls_isolate_shell_state(self):
        (self.work / "sub").mkdir()
        action = (
            'let first = run("cd sub; export SHELL_TEST_VALUE=changed; pwd; exit 8"); '
            'let second = run("pwd; printf \'%s\\\\n\' \\"$SHELL_TEST_VALUE\\"; cat"); '
            'require first.Status == 8 && second.Status == 0 '
            f'&& first.Stdout == {json.dumps(str(self.work / "sub") + chr(10))} '
            f'&& second.Stdout == {json.dumps(str(self.work) + chr(10) + "inherited" + chr(10))} '
            'else Throw(!Mismatch); return 0;'
        )
        for tool, env in self.tools(spec(action, ".import std.shell"), standard="c99"):
            with self.subTest(tool=tool):
                env["SHELL_TEST_VALUE"] = "inherited"
                self.assertEqual(self.call(*tool, env=env).stdout, "0\n")

    def test_status_diagnostics_and_large_capture(self):
        cases = [
            ("true", 'out.Status == 0 && out.Stdout == "" && out.Stderr == ""'),
            ("true | false", "out.Status == 1"),
            ("if true; then", 'out.Status == 2 && out.Stderr != ""'),
            ("llsynth-test-command-does-not-exist", "out.Status == 127"),
            (r"head -c 131072 /dev/zero | tr '\000' x",
             'out.Status == 0 && len(out.Stdout) == 131072 && out.Stderr == ""'),
        ]
        action = " ".join(
            f'if true {{ let out = Sh:run({json.dumps(source)}); require {check} else Throw(!Mismatch); }}'
            for source, check in cases
        ) + " return 0;"
        for tool, env in self.tools(spec(action)):
            with self.subTest(tool=tool):
                self.assertEqual(self.call(*tool, env=env).stdout, "0\n")

    def test_bridge_errors_use_shell_error(self):
        printf = shutil.which("printf")
        self.assertIsNotNone(printf)
        scripts = [
            (r"head -c 1 /dev/zero", "shell output is not NUL-free UTF-8"),
            (shlex.quote(printf) + r" '\377'", "shell output is not NUL-free UTF-8"),
            (r"head -c 8388609 /dev/zero | tr '\000' x", "a stream exceeds 8 MiB"),
            ("true", "cannot start libsh"),
        ]
        for source, diagnostic in scripts:
            action = f'return Sh:run({json.dumps(source)}).Status;'
            for tool, env in self.tools(spec(action)):
                with self.subTest(source=source, tool=tool):
                    if source == "true":
                        env["LLSYNTH_LIBSH"] = str(self.work / "missing-libsh")
                    err = self.call(*tool, env=env, ok=False).stderr
                    self.assertIn("shell bridge:", err)
                    self.assertIn(diagnostic, err)


if __name__ == "__main__":
    unittest.main()
