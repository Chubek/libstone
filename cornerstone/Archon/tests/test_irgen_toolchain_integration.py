"""Exercise the CLI-generated drivers with real assemblers and linkers."""

import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon import database
from archon.compiler import compile_file

from test_irgen_native import ELF_HEADER, EXAMPLE, RV64IM, TMP_ROOT, unicorn_exit_status


ROOT = Path(__file__).resolve().parents[1]
AMD64 = ROOT / "stdspec/amd64/root.arch"
START_X86 = """\
.section .text._start,"ax",@progbits
.globl _start
_start:
    call exit_code
    mov %eax, %edi
    mov $60, %eax
    syscall
.section .note.GNU-stack,"",@progbits
"""
CODE_X86 = """\
.section .text.exit_code,"ax",@progbits
.globl exit_code
exit_code:
    mov $42, %eax
    ret
.section .text.unused,"ax",@progbits
.globl unused
unused:
    mov $17, %eax
    ret
.section .note.GNU-stack,"",@progbits
"""
START_NASM = """\
section .text
global _start
_start:
    mov edi, 42
    mov eax, 60
    syscall
"""
LAYOUT = """\
ENTRY(_start)
SECTIONS {
    . = 0x401000;
    .text : { *(.text .text.*) }
    /DISCARD/ : { *(.note.GNU-stack) }
}
"""
LTO_SOURCES = (
    ("entry", "extern int twice(int);\nint exit_code(void) { return twice(21); }\n"),
    ("twice", "int twice(int x) { return x+x; }\n"),
)


class IRToolchainIntegration(unittest.TestCase):
    def setUp(self):
        TMP_ROOT.mkdir(parents=True, exist_ok=True)
        work = tempfile.TemporaryDirectory(prefix="archon-irgen-tools-", dir=TMP_ROOT)
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)
        self.generated = self.work / "generated scripts"
        self.host_db = self.work / "target.archdb"
        database.save(compile_file(AMD64), self.host_db)

    def run_command(self, *args, status=0, cwd=None, input=None):
        result = subprocess.run(
            args, cwd=cwd or self.work, input=input, capture_output=True, timeout=90,
        )
        self.assertEqual(result.returncode, status, f"{args!r}\n{result.stdout!r}\n{result.stderr!r}")
        return result.stdout

    def generate(self, *args, target_db=None, directory=None):
        output = directory or self.generated
        self.run_command(
            ROOT / "tools/archon-irgen", "gated", target_db or self.host_db,
            "--no-oracle", *args, "-o", output,
        )
        return output

    def create_text(self, name, text):
        path = self.work / name
        path.write_text(text, encoding="utf-8")
        return path

    def host_elf(self, tool):
        if not sys.platform.startswith("linux") or platform.machine() not in ("x86_64", "amd64"):
            self.skipTest("freestanding execution requires Linux x86-64")
        if not shutil.which(tool):
            self.skipTest(f"{tool} is not available")

    def assembly_objects(self, scripts):
        result = []
        for name, text in (("-start ; a.s", START_X86), ("@exit-code b.s", CODE_X86)):
            source = self.create_text(name, text)
            output = source.with_suffix(".o")
            # Deliberately use relative basenames that a provider might
            # otherwise interpret as options or a response-file directive.
            self.run_command(scripts / "Assembler.sh", "-o", output, "--", source.name)
            result.append(output)
        return result

    def test_gas_and_gnu_ld_with_explicit_layout(self):
        self.host_elf("as")
        self.host_elf("ld.bfd")
        self.host_elf("readelf")
        script = self.create_text("layout ; not a command.ld", LAYOUT)
        out = self.generate("--assembler", "gas", "--linker", "gnu", "--linker-script", script)
        self.assertFalse((out / "AsmOPT.lua").exists())
        self.assertFalse((out / "LTO.lua").exists())
        objects = self.assembly_objects(out)
        exe = self.work / "target ; run"
        self.run_command(out / "Linker.sh", "-o", exe, "--", *(path.name for path in objects))
        self.assertEqual(ELF_HEADER.unpack_from(exe.read_bytes())[4], 0x401000)
        self.run_command(exe, status=42)

    def test_gas_and_gnu_ld_output_runs_in_generated_sandbox(self):
        for tool in ("as", "ld.bfd", "readelf", "cc"):
            self.host_elf(tool)
        out = self.generate("--assembler", "gas", "--linker", "gnu")
        objects = self.assembly_objects(out)
        exe = self.work / "target ; sandbox"
        self.run_command(out / "Linker.sh", "-o", exe, "--entry", "_start", *objects)
        runner = self.work / "run-sandbox"
        self.run_command(
            "cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            out / "RunSandbox.c", "-o", runner,
        )
        result = subprocess.run(
            [runner, "--", f"./{exe.name}"], cwd=self.work, capture_output=True, timeout=10,
            env={**os.environ, "LC_ALL": "C"},
        )
        unavailable = (
            (b"prepare launcher descriptors", b"Function not implemented"),
            (b"create executable memfd", b"Invalid argument"),
            (b"create executable memfd", b"Function not implemented"),
            (b"create executable memfd", b"Permission denied"),
            (b"create executable memfd", b"Operation not permitted"),
            (b"restrict filesystem", b"Operation not supported"),
            (b"restrict filesystem", b"Function not implemented"),
            (b"restrict filesystem", b"Permission denied"),
            (b"install seccomp filter", b"Permission denied"),
            (b"install seccomp filter", b"Operation not permitted"),
        )
        if result.returncode == 126 and any(
            stage in result.stderr and reason in result.stderr for stage, reason in unavailable
        ):
            self.skipTest(os.fsdecode(result.stderr).strip())
        self.assertEqual(result.returncode, 42, (result.stdout, result.stderr))

    def test_nasm_and_gold(self):
        self.host_elf("nasm")
        self.host_elf("ld.gold")
        self.host_elf("readelf")
        out = self.generate("--assembler", "nasm", "--linker", "gold", "--no-lto")
        source = self.create_text("entry.asm", START_NASM)
        obj, exe = self.work / "entry.o", self.work / "run-gold"
        self.run_command(out / "Assembler.sh", "-o", obj, source)
        self.run_command(out / "Linker.sh", "-o", exe, obj)
        self.run_command(exe, status=42)

    def lto_objects(self, scripts, compiler, suffix):
        start = self.create_text("start.s", START_X86)
        obj = self.work / "start.o"
        self.run_command(scripts / "Assembler.sh", "-o", obj, start)
        inputs = [obj]
        for name, source in LTO_SOURCES:
            bitcode = self.work / f"{name}.{suffix}"
            self.run_command(
                compiler, "-O2", "-flto", "-ffreestanding", "-fno-pie", "-fno-stack-protector", "-c",
                self.create_text(f"{name}.c", source), "-o", bitcode,
            )
            inputs.append(bitcode)
        return inputs

    def check_lto_executable(self, exe):
        self.run_command(exe, status=42)
        if nm := shutil.which("nm"):
            symbols = self.run_command(nm, exe)
            self.assertIn(b" exit_code\n", symbols)
            self.assertNotIn(b" twice\n", symbols)

    def test_lld_with_real_llvm_bitcode_inputs(self):
        for tool in ("as", "clang", "ld.lld", "readelf"):
            self.host_elf(tool)
        out = self.generate("--assembler", "gas", "--linker", "lld", "--lto")
        self.assertFalse((out / "LTO.lua").exists())
        inputs = self.lto_objects(out, "clang", "bc")
        for bitcode in inputs[1:]:
            self.assertTrue(bitcode.read_bytes().startswith(b"BC\xc0\xde"))
        exe = self.work / "run-lto"
        self.run_command(out / "Linker.sh", "-o", exe, *inputs)
        self.check_lto_executable(exe)

    def test_gnu_ld_with_real_gcc_lto_objects(self):
        for tool in ("as", "gcc", "ld.bfd", "readelf"):
            self.host_elf(tool)
        plugin = Path(os.fsdecode(self.run_command("gcc", "-print-file-name=liblto_plugin.so")).strip())
        if not plugin.is_file():
            self.skipTest("GCC lacks liblto_plugin.so")
        out = self.generate("--linker", "gnu", "--lto", "--lto-driver", "gcc")
        self.assertFalse((out / "LTO.lua").exists())
        inputs = self.lto_objects(out, "gcc", "o")
        self.assertIn(b".gnu.lto_", self.run_command("readelf", "-WS", inputs[1]))
        exe = self.work / "run-gcc-lto"
        self.run_command(out / "Linker.sh", "-o", exe, "--entry", "_start", *inputs)
        self.check_lto_executable(exe)

    def test_llvm_as_and_lld_run_native_rv64_output(self):
        for tool in ("llvm-mc", "ld.lld", "cmake", "cc", "readelf"):
            if not shutil.which(tool):
                self.skipTest(f"{tool} is not available")
        if b"riscv64" not in self.run_command("llvm-mc", "--version"):
            self.skipTest("llvm-mc lacks RV64")
        path = self.work / "rv64im.archdb"
        database.save(compile_file(RV64IM), path)
        out = self.generate(
            "--assembler", "llvm", "--linker", "lld", "--profile", "native", target_db=path,
        )
        build = self.work / "native-build"
        self.run_command("cmake", "-S", out, "-B", build)
        self.run_command("cmake", "--build", build, "--target", "archon-backend")
        asm = self.work / "factorial.s"
        asm.write_bytes(self.run_command(
            build / "archon-backend", "--input", EXAMPLE / "factorial.mir",
        ))
        entry = self.create_text("rv-start.s", """\
    .text
    .globl _start
_start:
    addi a0, zero, 5
    call factorial
    addi a7, zero, 93
    ecall
""")
        inputs = []
        for source in (entry, asm):
            obj = source.with_suffix(".o")
            self.run_command(out / "Assembler.sh", "-o", obj, source)
            header = ELF_HEADER.unpack_from(obj.read_bytes())
            self.assertEqual((header[1], header[2], header[7]), (1, 243, 0))
            inputs.append(obj)
        exe = self.work / "run-rv64"
        self.run_command(out / "Linker.sh", "-o", exe, *inputs)
        ident, kind, machine, _, entry, *_ = ELF_HEADER.unpack_from(exe.read_bytes())
        self.assertEqual((ident[:7], kind, machine), (b"\x7fELF\x02\x01\x01", 2, 243))
        self.assertTrue(entry)
        try:
            import unicorn
            from unicorn import riscv_const
        except ImportError:
            self.skipTest("linked executable; execution requires Unicorn with RISC-V support")
        if not unicorn.uc_arch_supported(unicorn.UC_ARCH_RISCV):
            self.skipTest("linked executable; Unicorn lacks RISC-V support")
        self.assertEqual(unicorn_exit_status(exe, unicorn, riscv_const), 120)

    def test_generated_yld_hook_drops_dead_function_across_objects(self):
        self.host_elf("as")
        self.host_elf("readelf")
        yld = os.environ.get("ARCHON_YLD") or shutil.which("qobjld")
        nm = shutil.which("nm")
        if not yld or not nm:
            self.skipTest("set ARCHON_YLD to qobjld built with --lto-script; nm is also needed")
        if b"--lto-script" not in self.run_command(yld, "--help"):
            self.skipTest("qobjld does not advertise --lto-script")
        out = self.generate("--assembler", "gas", "--linker", "yld", "--lto", "--linker-program", yld)
        self.assertTrue((out / "LTO.lua").is_file())
        objects = self.assembly_objects(out)
        exe = self.work / "run-yld"
        self.run_command(out / "Linker.sh", "-o", exe, *objects)
        symbols = self.run_command(nm, exe)
        self.assertIn(b" exit_code\n", symbols)
        self.assertNotIn(b" unused\n", symbols)
        self.run_command(exe, status=42)

        # Layout Lua is a separate extension channel passed to -T.
        layout = self.create_text("user layout.lua", """\
qld.entry('_start')
qld.memory('TEXT', '0x501000', '4K', 'rx')
qld.section('.text', {patterns={'.text', '.text.*'}, region='TEXT'})
""")
        self.run_command(out / "Linker.sh", "-o", exe, "--script", layout, *objects)
        self.assertEqual(ELF_HEADER.unpack_from(exe.read_bytes())[4], 0x501000)
        self.assertNotIn(b" unused\n", self.run_command(nm, exe))
        self.run_command(exe, status=42)

        unoptimized = self.generate(
            "--linker", "yld", "--linker-program", yld, "--linker-script", layout,
            directory=self.work / "yld without lto",
        )
        self.assertFalse((unoptimized / "LTO.lua").exists())
        self.run_command(unoptimized / "Linker.sh", "-o", exe, *objects)
        self.assertEqual(ELF_HEADER.unpack_from(exe.read_bytes())[4], 0x501000)
        self.assertIn(b" unused\n", self.run_command(nm, exe))
        self.run_command(exe, status=42)

    def test_generated_sandstone_hook_runs_after_macro_expansion(self):
        assembler = os.environ.get("ARCHON_TEST_SANDSTONE_AS")
        bank = os.environ.get("ARCHON_TEST_SANDSTONE_BANK")
        if not assembler or not bank:
            self.skipTest("set ARCHON_TEST_SANDSTONE_AS and ARCHON_TEST_SANDSTONE_BANK")
        path = self.work / "rv64im.archdb"
        database.save(compile_file(RV64IM), path)
        out = self.generate(
            "--assembler", "sandstone", "--assembler-program", assembler,
            "--sandstone-bank", bank, "--sandstone-syntax", "default", target_db=path,
        )
        hook = out / "AsmOPT.lua"
        # The alias has identical bytes, so observe that the generated hook
        # actually rewrote the macro-expanded input before comparing bytes.
        with hook.open("a", encoding="utf-8") as handle:
            handle.write("""
local generated_opt = asm_optimize
function asm_optimize(source, name)
    assert(not source:find(".macro", 1, true), "hook ran before expansion")
    local output = generated_opt(source, name)
    assert(output ~= source and output:find("nop", 1, true), "generated alias was not used")
    return output
end
""")
        source = self.create_text("sandstone input.s", """\
.macro padding
addi x0, x0, 0
.endmacro
.text
padding
add x1, x2, x3
""")
        image = self.work / "out.bin"
        self.run_command(out / "Assembler.sh", "-o", image, source)
        self.assertEqual(image.read_bytes(), bytes.fromhex("13000000 b3003100"))

    def test_cli_protects_configured_layout_and_rejects_option_conflicts(self):
        self.generated.mkdir()
        path = self.generated / "layout.ld"
        path.write_text(LAYOUT, encoding="ascii")
        args = (
            ROOT / "tools/archon-irgen", "gated", self.host_db, "--linker", "gnu",
            "-o", self.generated,
        )
        result = subprocess.run(
            [*args, "--linker-script", path, "--force"], cwd=self.work, capture_output=True, timeout=25,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"overwrite input", result.stderr)
        self.assertEqual(list(self.generated.iterdir()), [path])
        self.assertEqual(path.read_text(encoding="ascii"), LAYOUT)
        if os.name == "posix":
            outside = self.create_text("outside.ld", LAYOUT)
            path.unlink()
            path.symlink_to(outside)
            rejected = self.run_command_rejected(*args, "--linker-script", path, "--force")
            self.assertIn(b"overwrite input", rejected)
            self.assertTrue(path.is_symlink())
        rejected = self.run_command_rejected(*args, "--no-lto", "--option", "lto=true")
        self.assertIn(b"specified by both --no-lto and --option", rejected)
        rejected = self.run_command_rejected(*args, "--assembler", "llvm", "--option", 'assembler="gas"')
        self.assertIn(b"specified by both --assembler and --option", rejected)
        rejected = self.run_command_rejected(*args, "--lto", "--no-lto")
        self.assertIn(b"not allowed with argument", rejected)

    def run_command_rejected(self, *args):
        result = subprocess.run(args, cwd=self.work, capture_output=True, timeout=25)
        self.assertNotEqual(result.returncode, 0, f"{args!r}")
        return result.stderr


if __name__ == "__main__":
    unittest.main()
