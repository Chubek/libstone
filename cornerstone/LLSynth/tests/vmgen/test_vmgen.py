#!/usr/bin/env python3
"""Exercise .vmgen and compile/run a tape against the real VMWeave runtime."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts/llsynth.pl"
EXAMPLE = (ROOT / "examples/vmgen/pairs.llsynth").read_text()
VMWEAVE = os.environ.get("VMWEAVE_EXECUTABLE") or shutil.which("vmweave-cli")
CC = shutil.which("cc")


def run(*args, data=None, env=None):
    return subprocess.run(args, input=data, text=True, capture_output=True, timeout=45, env=env)


class VMGenTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory(prefix="llsynth-vmgen-test-")
        self.addCleanup(folder.cleanup)
        self.path = Path(folder.name)
        self.source = self.path / "pairs.llsynth"
        self.source.write_text(EXAMPLE)
        self.env = dict(os.environ)
        if VMWEAVE:
            self.env["VMWEAVE_EXECUTABLE"] = VMWEAVE

    def gen(self, *args, env=None, target="vmweave"):
        return run(GENERATOR, "--mode", "both", "--program-target", target, *args, self.source,
                   env=self.env if env is None else env)

    def good(self, result):
        self.assertEqual(0, result.returncode, result.stderr)
        return result.stdout

    def bad(self, result, message):
        self.assertNotEqual(0, result.returncode, result.stdout)
        self.assertIn(message, result.stderr)

    def test_frontend_errors_without_vmweave(self):
        env = dict(self.env, VMWEAVE_EXECUTABLE=str(self.path / "missing-cli"))
        self.bad(self.gen("--check", env=env), "VMWEAVE_EXECUTABLE")
        start, rest = EXAMPLE.split(".vmgen for Tape\n", 1)
        _, tail = rest.split("\n.end\n", 1)
        self.source.write_text(start + tail)
        self.bad(self.gen("--check", env=env), "no backend defined for program target 'vmweave'")
        self.source.write_text(start + ".vmgen for Tape\n" + "return {}\n")
        self.bad(self.gen("--check", env=env), "unterminated .vmgen block")
        block = ".vmgen for Tape\nreturn {}\n.end\n"
        self.source.write_text(start + block * 2 + tail)
        self.bad(self.gen("--check", env=env), "only one .vmgen block")
        self.source.write_text(start + block.replace("for Tape", "Tape") + tail)
        self.bad(self.gen("--check", env=env), "expected .vmgen for IR")
        self.source.write_text(start + block.replace("for Tape", "for Absent") + tail)
        self.bad(self.gen("--check", env=env), "requires a nonempty IR 'Absent'")
        (self.path / "fragment.llsynth").write_text(".vmgen for Tape\nreturn {}\n")
        self.source.write_text(start + '.include "fragment.llsynth"\n.end\n' + tail)
        self.bad(self.gen("--check", env=env), ".vmgen block cannot cross a file boundary")
        (self.path / "Imported.llsynth").write_text(".library Imported\n" + block)
        self.source.write_text('.import Imported\n' + start + tail)
        self.bad(self.gen("--check", env=env), ".vmgen is entry-specification configuration")

    @unittest.skipUnless(VMWEAVE, "set VMWEAVE_EXECUTABLE to a built vmweave-cli")
    def test_contract_and_lua_diagnostics(self):
        self.good(self.gen("--check"))
        def reject(text, message):
            self.source.write_text(text)
            result = self.gen("--check", "--diagnostics=json")
            self.bad(result, message)
            self.assertEqual(str(self.source), json.loads(result.stderr)["location"]["file"])

        reject(EXAMPLE.replace('name = "MIX"', 'name = "OTHER"'), "no VMWeave instruction named 'MIX'")
        reject(EXAMPLE.replace("const 2; mul;", "dup;"), "stack effect mismatch")
        reject(EXAMPLE.replace('abstract = "add; const 2; mul;"', 'abstract = "add; halt;"'),
               "requires a VMWeave instruction with next control flow")
        reject(EXAMPLE.replace('operands = { "cell" }, abstract = "operand 0;"',
                               'operands = {}, abstract = "const 0;"'), "operand count or kind mismatch")
        reject(EXAMPLE.replace('cell = "int64_t"', 'cell = "intptr_t"'), "requires VMWeave int64_t cells")
        reject(EXAMPLE.replace('components = { "insncode", "dispatch" }', 'components = { "insncode" }'),
               "requires the insncode and dispatch")
        reject(EXAMPLE.replace('opcode = 29', 'opcode = 7'), "duplicate opcode")
        reject(EXAMPLE.replace("return v.vm {", "error('lua marker')\nreturn v.vm {"), "lua marker")

    @unittest.skipUnless(VMWEAVE and CC, "requires vmweave-cli and a C compiler")
    def test_generated_python_and_c_execute_identical_tapes(self):
        py, tool_c, tool = (self.path / name for name in ("pairs.py", "compiler.c", "compiler"))
        self.good(self.gen("-T", "python3", "-o", py))
        self.good(self.gen("-T", "c99", "-o", tool_c))
        self.good(run(CC, "-std=c99", tool_c, "-lm", "-o", tool))
        without_vm = dict(self.env, VMWEAVE_EXECUTABLE=str(self.path / "missing-cli"))
        outputs = []
        for cmd in ((sys.executable, py), (tool,)):
            self.assertEqual("42\n", self.good(run(*cmd, "--interpret", data="11 10", env=without_vm)))
            outputs.append(self.good(run(*cmd, "--compile", data="11 10", env=without_vm)))
            self.bad(run(*cmd, "--compile", "--target", "bytecode", data="11 10"), "BYTECODE")
        self.assertEqual(*outputs)
        self.assertIn("{7u, 1, {INT64_C(11)}}", outputs[0])
        self.assertIn("{29u, 0, {0}}", outputs[0])
        self.assertIn("PairsVM_run", outputs[0])
        vm_c, vm = self.path / "tape.c", self.path / "vm"
        vm_c.write_text(outputs[0])
        self.good(run(CC, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", vm_c, "-o", vm))
        self.assertEqual("42\n", self.good(run(vm, env=without_vm)))

        for cmd in ((sys.executable, py), (tool,)):
            vm_c.write_text(self.good(run(*cmd, "--compile", data="9223372036854775807 1")))
            self.good(run(CC, "-std=c99", vm_c, "-o", vm))
            self.bad(run(vm), "VMWeave: status -10 at instruction 2")

        self.source.write_text(EXAMPLE.replace("add; const 2; mul;", "drop; const 0; add;"))
        self.good(self.gen("-T", "python3", "-o", py))
        vm_c.write_text(self.good(run(sys.executable, py, "--compile", data="-9223372036854775808 0")))
        self.assertIn("INT64_MIN", vm_c.read_text())
        self.good(run(CC, "-std=c99", "-pedantic-errors", vm_c, "-o", vm))
        self.assertEqual("-9223372036854775808\n", self.good(run(vm)))

    @unittest.skipUnless(VMWEAVE, "set VMWEAVE_EXECUTABLE to a built vmweave-cli")
    def test_opaque_included_lua_and_explicit_target(self):
        start, rest = EXAMPLE.split(".vmgen for Tape\n", 1)
        lua, tail = rest.split("\n.end\n", 1)
        fragment = self.path / "vm.llsynth"
        fragment.write_text(
            '.vmgen for Tape\n--[=[\n.include "does-not-exist"\n.start Absent\n.rule Absent\n'
            '.function argument(ghost: Int)->Int\n.import "also-absent"\n]=]\n'
            + lua + "\n.end\n"
        )
        (self.path / "Imported.llsynth").write_text(".library Imported\n.type %Number = Int\n")
        self.source.write_text(
            ".import Imported\n" + start + '.include "vm.llsynth"\n' + tail
            + '\n.backend text for Tape\n.emit PUSH(value) => "push ${value}\\n"\n'
              '.emit MIX => "mix\\n"\n.end\n'
        )
        py = self.path / "explicit.py"
        self.good(self.gen("-o", py, "--manifest", self.path / "manifest.json", target="text"))
        output = run(sys.executable, py, "--compile", data="11 10")
        self.assertEqual("push 11\npush 10\nmix\n", self.good(output))
        output = run(sys.executable, py, "--compile", "--target", "vmweave", data="11 10")
        self.assertIn("PairsVM_run", self.good(output))
        deps = json.loads((self.path / "manifest.json").read_text())["dependencies"]
        paths = {entry["path"] for entry in deps}
        self.assertIn(str(fragment), paths)
        self.assertIn(str(Path(shutil.which(VMWEAVE) or VMWEAVE).resolve()), paths)

    @unittest.skipUnless(VMWEAVE, "set VMWEAVE_EXECUTABLE to a built vmweave-cli")
    def test_rewrites_and_vm_capacity(self):
        py = self.path / "rewrite.py"
        self.source.write_text(EXAMPLE.replace("capacity = 64", "capacity = 2").replace(
            "Tape.PUSH($a), Tape.PUSH($b), Tape.MIX",
            "Tape.PUSH(0), Tape.PUSH($a), Tape.PUSH($b), Tape.MIX, Tape.MIX"
        ))
        self.good(self.gen("-o", py))
        self.bad(run(sys.executable, py, "--compile", data="11 10"), "VM stack capacity exceeded")
        self.source.write_text(self.source.read_text() +
                               "\n.rewrite Fold: Tape\n.match PUSH(a), PUSH(b), MIX\n"
                               ".replace PUSH((a + b) * 2)\n.end\n")
        self.good(self.gen("-o", py))
        compiled = self.good(run(sys.executable, py, "--compile", data="11 10"))
        self.assertIn("{7u, 1, {INT64_C(84)}}", compiled)
        self.assertNotIn("{29u, 0, {0}}", compiled)
        self.source.write_text(EXAMPLE.replace(
            "Tape.PUSH($a), Tape.PUSH($b), Tape.MIX", "Tape.PUSH($a), Tape.PUSH($b)"
        ))
        self.good(self.gen("-o", py))
        self.bad(run(sys.executable, py, "--compile", data="11 10"), "expected one final Int result")


if __name__ == "__main__":
    unittest.main()
