"""Generated assembler argument, target and byte-level integration tests."""

import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen import assembler_script, build_context, pipeline_stages
from archon.runtime import Target


ROOT = Path(__file__).resolve().parents[1]


class AssemblerScriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        specs = ROOT / "stdspec"
        cls.rv32 = compile_file(specs / "riscv/rv32i.arch")
        cls.rv64 = compile_file(specs / "riscv/rv64im.arch")
        cls.amd64 = compile_file(specs / "amd64/root.arch")
        cls.jvm = compile_file(specs / "jvm/root.arch")

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.generated = self.root / "generated 'output\nfiles"

    def generate(self, db, **options):
        files = assembler_script.emit({"database": db, "options": options})
        for name, data in files.items():
            path = self.generated / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data, encoding="utf-8")
        return files

    def tool(self, name):
        command = shutil.which(name)
        if command is None:
            self.skipTest(f"{name} unavailable")
        return command

    def run_wrapper(self, *args, cwd=None, **options):
        return subprocess.run(
            ["sh", str(self.generated / "Assembler.sh"), *args],
            cwd=cwd or self.root, capture_output=True, text=True, timeout=10, **options,
        )

    def write_source(self, filename, source):
        (self.root / filename).write_text(source, encoding="utf-8")
        return filename

    def fake_tool(self):
        path = self.root / "tool with 'quote' $(touch BUG)"
        path.write_text(
            f"#!{sys.executable}\nimport json, pathlib, sys\n"
            "args = sys.argv[1:]\n"
            "print(json.dumps(args))\n"
            "staged = args[args.index('-o') + 1] if '-o' in args else args[-1]\n"
            "output = pathlib.Path(staged)\n"
            "output.write_bytes(b'assembled')\n"
            "output.chmod(0o751)\n",
            encoding="utf-8",
        )
        path.chmod(0o755)
        return str(path)

    def assert_elf_text(self, path, machine, expected):
        data = path.read_bytes()
        self.assertEqual(data[:7], b"\x7fELF\x02\x01\x01")
        self.assertEqual(int.from_bytes(data[16:18], "little"), 1)  # ET_REL
        self.assertEqual(int.from_bytes(data[18:20], "little"), machine)
        (section_base,) = struct.unpack_from("<Q", data, 40)
        section_size, section_count, name_section = struct.unpack_from("<HHH", data, 58)

        def section(index):
            return struct.unpack_from("<IIQQQQIIQQ", data, section_base + index * section_size)

        names = section(name_section)
        strings = data[names[4]:names[4] + names[5]]
        for index in range(section_count):
            name, _, _, _, offset, size, *_ = section(index)
            if strings[name:strings.index(b"\0", name)] == b".text":
                self.assertEqual(data[offset:offset + size], expected)
                break
        else:
            self.fail(f"{path} has no .text section")

    def assert_staged_path(self, staged, output):
        destination = self.root / output
        temporary_output = self.root / staged
        self.assertEqual(temporary_output.name, destination.name)
        self.assertEqual(temporary_output.parent.parent, destination.parent)
        self.assertTrue(temporary_output.parent.name.startswith(".archon-as."))
        self.assertFalse(temporary_output.parent.exists())
        self.assertEqual(destination.read_bytes(), b"assembled")
        self.assertEqual(destination.stat().st_mode & 0o777, 0o751)

    def test_discovery_defaults_and_uses_the_selected_archdb(self):
        self.assertIn(assembler_script, pipeline_stages())
        context = build_context("anf", [self.rv32, self.rv64], options={"target": "RISCV64IM"})
        files = assembler_script.emit(context)
        self.assertNotIn("AsmOPT.lua", files)
        self.assertNotIn("ArchonAssembler.py", files)
        script = files["Assembler.sh"]
        self.assertIn("riscv64-linux-gnu-as", script)
        self.assertIn("-march=rv64im -mabi=lp64", script)
        self.assertNotIn("--64", script)
        reverse = assembler_script.emit({
            "architectures": [self.rv64, self.rv32],
            "options": {"target": "RISCV64IM"},
        })
        self.assertEqual(reverse["Assembler.sh"], script)

    def test_option_validation(self):
        cases = (
            (self.amd64, {"assembler": ["gas"]}, "nonempty string"),
            (self.amd64, {"assembler": "not-an-assembler"}, "unknown assembler"),
            (self.amd64, {"assembler_program": "--evil"}, "not an option"),
            (self.amd64, {"assembler_program": "x\0y"}, "control characters"),
            (self.amd64, {"assembler_format": "bin"}, "gas assembler_format"),
            (self.amd64, {"assembler": "nasm", "assembler_format": "elf32"}, "32-bit NASM"),
            (self.rv64, {"assembler": "fasm"}, "requires an x86"),
            (self.jvm, {"assembler": "llvm"}, "requires 'llvm_triple'"),
            (self.rv64, {"assembler": "llvm", "llvm_triple": "x86_64-linux-gnu"}, "does not match"),
            (self.rv64, {"assembler": "llvm", "llvm_mattr": "+m,-m"}, "repeat"),
            (self.rv64, {"assembler": "llvm", "llvm_mattr": "m -x"}, "invalid assembler option"),
            (self.rv64, {"assembler": "sandstone"}, "sandstone_bank"),
            (self.rv64, {"assembler": "sandstone", "sandstone_archml": "t.aml",
                         "sandstone_syntax": "default"}, "requires a sandstone_bank"),
            (self.rv64, {"assembler": "sandstone", "sandstone_bank": "bank",
                         "assembler_format": "elf"}, "bin, obj"),
            (self.rv64, {"sandstone_bank": "bank"}, "requires assembler=sandstone"),
            (None, {"assembler": "archon"}, "requires a selected ArchDB"),
        )
        for db, options, message in cases:
            with self.subTest(options=options), self.assertRaisesRegex(ArchonError, re.escape(message)):
                self.generate(db, **options)

    def test_argv_controls_are_literal_and_status_is_not_masked(self):
        program = self.fake_tool()
        self.generate(self.rv64, assembler_program=program)
        source = "- source 'one'"
        other = "@other $(touch BUG); \nline"
        output = "-output; $(touch BUG)"
        result = self.run_wrapper("-o", output, "--", source, other)
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = json.loads(result.stdout)
        self.assertEqual(argv, [
            "-march=rv64im", "-mabi=lp64", "-o", argv[3], "./" + source, "./" + other,
        ])
        self.assert_staged_path(argv[3], output)
        self.assertFalse((self.root / "BUG").exists())
        self.generate(self.rv64, assembler_program="/does/not/exist")
        result = self.run_wrapper("-o", "a.o", "a.s")
        self.assertEqual(result.returncode, 127)
        self.assertIn("assembler_program", result.stderr)
        Path(program).write_text("#!/bin/sh\nexit 17\n", encoding="utf-8")
        self.generate(self.rv64, assembler_program=program)
        self.assertEqual(self.run_wrapper("-o", "a.o", "a.s").returncode, 17)

    def test_publication_requires_success_and_a_regular_output(self):
        tool = Path(self.fake_tool())
        self.generate(self.amd64, assembler_program=str(tool))
        source = self.write_source("a.s", ".byte 42\n")
        folder = self.root / "folder $(touch BUG)"
        folder.mkdir()
        output = folder / "result.o"
        output.write_bytes(b"previous build")
        relative = str(output.relative_to(self.root))
        for body, status, message in (
            ("printf partial > \"$3\"\nexit 17", 17, "failed (exit 17)"),
            ("exit 0", 2, "without producing a regular output"),
            ("ln -s ../result.o \"$3\"", 2, "without producing a regular output"),
        ):
            with self.subTest(body=body):
                # GNU invocation is --64 -o STAGED INPUT.
                tool.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
                result = self.run_wrapper("-o", relative, source)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertEqual(output.read_bytes(), b"previous build")
                self.assertFalse(list(folder.glob(".archon-as.*")))
        self.assertFalse((self.root / "BUG").exists())

    def test_bad_invocation_cannot_overwrite_an_input(self):
        self.generate(self.amd64, assembler_program="/does/not/exist")
        source = self.write_source("source.s", ".byte 42\n")
        (self.root / "out.o").hardlink_to(self.root / source)
        for args in ((), ("-o", "out.o", "--"), ("--bogus", "out.o", source), ("-o", "out.o", "")):
            with self.subTest(args=args):
                self.assertEqual(self.run_wrapper(*args).returncode, 2)
        result = self.run_wrapper("-o", "out.o", "./" + source)
        self.assertEqual(result.returncode, 2)
        self.assertIn("would overwrite", result.stderr)
        result = self.run_wrapper("-o", str(self.generated / "Assembler.sh"), source)
        self.assertEqual(result.returncode, 2)
        self.assertIn("would overwrite wrapper", result.stderr)
        self.assertEqual((self.root / source).read_text(encoding="utf-8"), ".byte 42\n")
        self.generate(self.jvm)
        result = self.run_wrapper("-o", "jvm.bin", source)
        self.assertEqual(result.returncode, 2)
        self.assertIn("no GNU assembler mapping", result.stderr)

    def test_output_cannot_alias_configured_or_bundled_files(self):
        program = self.fake_tool()
        bank = self.root / "bank.symas"
        bank.write_bytes(b"bank")
        archml = self.root / "vendor.aml"
        archml.write_bytes(b"profile")
        self.generate(self.rv64, assembler="sandstone", assembler_program=program,
                      sandstone_bank=str(bank), sandstone_archml=str(archml))
        hook = self.generated / "AsmOPT.lua"
        for path, role in ((Path(program), "selected assembler"), (bank, "bank"),
                           (archml, "archml"), (hook, "optimizer hook")):
            with self.subTest(role=role):
                before = path.read_bytes()
                alias = self.root / "alias"
                alias.symlink_to(path)
                result = self.run_wrapper("-o", str(alias), "a.s")
                self.assertEqual(result.returncode, 2)
                self.assertIn(f"would overwrite {role}", result.stderr)
                self.assertTrue(alias.is_symlink())
                self.assertEqual(path.read_bytes(), before)
                alias.unlink()
        self.generate(self.rv64, assembler="archon", assembler_program=sys.executable)
        for path, role in ((self.generated / "ArchonAssembler.py", "generated Archon assembler"),
                           (self.generated / "_archon_asm_runtime/runtime.py", "bundled Archon runtime")):
            with self.subTest(role=role):
                result = self.run_wrapper("-o", str(path), "a.s")
                self.assertEqual(result.returncode, 2)
                self.assertIn(f"would overwrite {role}", result.stderr)

    def test_gas_assembles_multiple_inputs_with_literal_paths(self):
        assembler = self.tool("as")
        version = subprocess.check_output([assembler, "--version"], text=True)
        if "GNU assembler" not in version or "x86_64" not in version:
            self.skipTest("requires an x86-64 GNU assembler")
        self.generate(self.amd64, assembler_program=assembler)
        left = self.write_source("-left ' file.s", ".text\n.globl lhs\nlhs:\n.byte 0x90\n")
        right = self.write_source("@right file.s", ".text\n.globl rhs\nrhs:\n.byte 0xc3\n")
        result = self.run_wrapper("-o", "@output file.o", "--", left, right)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_elf_text(self.root / "@output file.o", 62, b"\x90\xc3")

    def test_llvm_assembles_target_code_and_rejects_multiple_inputs(self):
        self.generate(self.rv64, assembler="llvm", assembler_program=self.tool("llvm-mc"))
        source = self.write_source("@rv64.s", "mul x1, x2, x3\n")
        result = self.run_wrapper("-o", "rv64.o", source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_elf_text(self.root / "rv64.o", 243, b"\xb3\x00\x31\x02")
        result = self.run_wrapper("-o", "bad.o", source, "other.s")
        self.assertEqual(result.returncode, 2)
        self.assertIn("exactly one", result.stderr)
        self.assertFalse((self.root / "bad.o").exists())

    def test_nasm_assembles_its_own_dialect(self):
        self.generate(self.amd64, assembler="nasm", assembler_program=self.tool("nasm"),
                      assembler_format="bin")
        source = self.write_source("-nasm source.s", "xor rax, rax\nret\n")
        result = self.run_wrapper("-o", "-nasm output.bin", "--", source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "-nasm output.bin").read_bytes(), b"\x48\x31\xc0\xc3")
        self.assertNotEqual(self.run_wrapper("-o", "multi.bin", source, source).returncode, 0)

    def test_fasm_uses_positional_input_and_output(self):
        self.generate(self.amd64, assembler="fasm", assembler_program=self.fake_tool())
        result = self.run_wrapper("-o", "@out.bin", "--", "-input assembly.asm")
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = json.loads(result.stdout)
        self.assertEqual(len(argv), 2)
        self.assertEqual(argv[0], "./-input assembly.asm")
        self.assert_staged_path(argv[1], "@out.bin")

    def test_fasm_assembles_when_installed(self):
        self.generate(self.amd64, assembler="fasm", assembler_program=self.tool("fasm"))
        source = self.write_source("fasm source.asm", "format binary\nuse64\nret\n")
        result = self.run_wrapper("-o", "fasm out.bin", source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "fasm out.bin").read_bytes(), b"\xc3")

    def test_archon_is_standalone_and_resolves_codec_labels(self):
        self.generate(self.rv64, assembler="archon", assembler_program=sys.executable)
        left = self.write_source("-first #.archs", "local: add x1, x2, x3\naddi x4, x0, @local")
        right = self.write_source("second .archs", "local: addi x5, x0, local\n")
        result = self.run_wrapper(
            "-o", "test.bin", "--", left, right,
            env={**os.environ, "PYTHONPATH": "/unavailable/checkout/archon"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        target = Target(self.rv64)
        expected = target.assemble_program("add x1, x2, x3\naddi x4, x0, -4\naddi x5, x0, 8\n")
        output = self.root / "test.bin"
        self.assertEqual(output.read_bytes(), expected)
        bad = self.write_source("invalid.archs", "addi x1, x2, unknown\n")
        result = self.run_wrapper("-o", "test.bin", bad)
        self.assertEqual(result.returncode, 1)
        self.assertIn("invalid.archs:1:1: unknown label", result.stderr)
        self.assertEqual(output.read_bytes(), expected)
        bad = self.write_source("gas.s", ".text\nadd x1, x2, x3\n")
        result = self.run_wrapper("-o", "test.bin", bad)
        self.assertEqual(result.returncode, 1)
        self.assertIn("does not match any instruction", result.stderr)

    def test_archon_supports_a_registerless_target(self):
        self.generate(self.jvm, assembler="archon", assembler_program=sys.executable)
        source = self.write_source("java.s", "iconst_1\niconst_2\niadd\n")
        result = self.run_wrapper("-o", "program.bytes", source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "program.bytes").read_bytes(), b"\x04\x05\x60")

    def test_sandstone_passes_bank_and_adjacent_hook(self):
        options = dict(
            assembler="sandstone", assembler_program=self.fake_tool(),
            sandstone_bank="@bank here ' $(touch BUG)", sandstone_syntax="default",
            assembler_format="obj",
        )
        generated = self.generate(self.rv64, **options)
        self.assertIn("riscv64_nop_alias", generated["AsmOPT.lua"])
        result = self.run_wrapper("-o", "program.obj.json", "--", "input a", "input b")
        self.assertEqual(result.returncode, 0, result.stderr)
        argv = json.loads(result.stdout)
        self.assertEqual(argv, [
            "--bank", "./" + options["sandstone_bank"], "--syntax", "default", "-f", "obj",
            "--asm-opt", str(self.generated / "AsmOPT.lua"), "-o", argv[9], "--",
            "input a", "input b",
        ])
        self.assert_staged_path(argv[9], "program.obj.json")
        self.assertFalse((self.root / "BUG").exists())
        result = self.run_wrapper("-o", options["sandstone_bank"], "test.s")
        self.assertEqual(result.returncode, 2)
        self.assertIn("would overwrite bank", result.stderr)
        (self.generated / "AsmOPT.lua").unlink()
        result = self.run_wrapper("-o", "program.obj.json", "test.s")
        self.assertEqual(result.returncode, 2)
        self.assertIn("missing adjacent AsmOPT.lua", result.stderr)
        for target, overrides in ((self.rv32, {}),
                                  (self.rv64, {"sandstone_syntax": "abi"}),
                                  (self.rv64, {"sandstone_syntax": "none"})):
            self.assertNotIn("riscv64_nop_alias", self.generate(target, **(options | overrides))["AsmOPT.lua"])
        self.assertNotIn("riscv64_nop_alias", self.generate(
            self.rv64, assembler="sandstone", sandstone_archml="archml profile.aml",
        )["AsmOPT.lua"])

    def test_sandstone_rewrite_is_exact_and_requires_bank_evidence(self):
        lua = self.tool("lua")
        self.generate(self.rv64, assembler="sandstone", sandstone_bank="bank.symas",
                      sandstone_syntax="default")
        test = r'''
dofile(arg[1])
local source = "\naddi x0, x0, 0\naddi x0,x0,0\n"
    .. "  addi x0, x0, 0\t\r\naddi\tx0,x0,0\n"
    .. "label: addi x0,x0,0\naddi x1,x0,0\naddi x0,x0,0 # keep\n"
    .. ".ascii \"addi x0,x0,0\"\naddi x0,x0,ZERO\naddi x0,x0,1"
assert(asm_optimize(source, "unit.s") == source)
asm_context = {arch = "riscv64", syntax = "default"}
assert(asm_optimize(source, "unit.s") == source)
asm_context.riscv64_nop_alias = true
asm_context.arch = "riscv32"
assert(asm_optimize(source, "unit.s") == source)
asm_context.arch = "riscv64"
asm_context.syntax = "abi"
assert(asm_optimize(source, "unit.s") == source)
asm_context.syntax = "default"
local expected = "\nnop\nnop\n" .. source:match("(  addi.*)")
assert(asm_optimize(source, "unit.s") == expected)
assert(asm_optimize("addi x0, x0, 0", "unit.s") == "nop")
'''
        path = self.root / self.write_source("test.lua", test)
        result = subprocess.run([lua, str(path), str(self.generated / "AsmOPT.lua")],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_sandstone_against_real_bank_if_configured(self):
        assembler = os.environ.get("ARCHON_TEST_SANDSTONE_AS")
        bank = os.environ.get("ARCHON_TEST_SANDSTONE_BANK")
        if not assembler or not bank:
            self.skipTest("requires the new sandstone-as and the shipped RV64 spec's compiled bank")
        self.generate(self.rv64, assembler="sandstone", assembler_program=str(Path(assembler).resolve()),
                      sandstone_bank=str(Path(bank).resolve()), sandstone_syntax="default")
        # Identical encoded bytes alone would also pass for an identity hook.
        hook = self.generated / "AsmOPT.lua"
        hook.write_text(hook.read_text(encoding="utf-8") + r'''
local generated_optimize = asm_optimize
function asm_optimize(source, name)
    local rewritten = generated_optimize(source, name)
    assert(rewritten ~= source and rewritten:find("\nnop\n", 1, true),
           "the bank's NOP rule was not applied to the expanded macro")
    return rewritten
end
''', encoding="utf-8")
        source = self.write_source("source path.s", ".text\n.macro no_op\naddi x0, x0, 0\n.endm\n"
                                                      "start:\nno_op\nend:\naddi x1, x0, 7\n")
        result = self.run_wrapper("-o", "out.bin", source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "out.bin").read_bytes(), b"\x13\0\0\0\x93\0\x70\0")


if __name__ == "__main__":
    unittest.main()
