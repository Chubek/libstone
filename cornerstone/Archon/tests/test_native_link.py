"""Static RV64 ELF linking, with an independent check of resolved references."""

import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen import generate_ir, write_generated_directory
from archon.irgen.objectwriter import assemble_object, link_executable

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "stdspec/riscv/rv64im.arch"
START = ROOT / "examples/native-backend/start.S"
IDENT = b"\x7fELF\x02\x01\x01"
EHDR = struct.Struct("<16sHHIQQQIHHHHHH")
PHDR = struct.Struct("<IIQQQQQQ")
SHDR = struct.Struct("<IIQQQQIIQQ")
SYM = struct.Struct("<IBBHQQ")

MAIN = """archon-mir 1
module 1 1 1
function archon_main i64 0 3 1 0
types ptr i64 i64
block 0 3
params
global_addr 0 datum
load 1 0 0
call 2 archon_adjust 1 1
ret 2
import archon_adjust i64 1 i64
global datum 1 init 40
end
"""
ADJUST = """archon-mir 1
module 1 1 0
function archon_adjust i64 1 2 1 0
types i64 i64
block 0 1
params
call 1 native_add2 1 0
ret 1
import native_add2 i64 1 i64
end
"""
TARGET = """\
extern long archon_main(void);
long native_add2(long x) { return x + 2; }
int target_main(void) { return archon_main() != 42; }
"""


def temporary_directory(prefix):
    scratch = Path("/tmp/opencode")
    return tempfile.TemporaryDirectory(prefix=prefix, dir=scratch if scratch.is_dir() else None)


def run(*argv):
    result = subprocess.run(argv, capture_output=True, timeout=60)
    if result.returncode:
        raise AssertionError(f"{argv!r} failed: {result.stderr.decode(errors='replace')}")
    return result.stdout


class LinkGuardTests(unittest.TestCase):
    def setUp(self):
        work = temporary_directory("archon-link-guards-")
        self.addCleanup(work.cleanup)
        self.root = Path(work.name)
        self.obj = self.root / "in.o"
        self.dest = self.root / "out"
        self.dest.write_bytes(b"previous destination")
        names = b"\0.shstrtab\0"
        self.obj.write_bytes(EHDR.pack(
            IDENT, 1, 243, 1, 0, 0, 64, 0, 64, 0, 0, 64, 2, 1,
        ) + bytes(64) + SHDR.pack(1, 3, 0, 0, 192, len(names), 0, 0, 1, 0) + names)
        tool = patch("archon.irgen.objectwriter._tool", return_value="/trusted/ld.lld")
        tool.start()
        self.addCleanup(tool.stop)

    @patch("archon.irgen.objectwriter.subprocess.run")
    def test_rejects_invalid_inputs_before_invoking_linker(self, invoke):
        baseline = self.obj.read_bytes()
        for offset, replacement, message in (
            (4, b"\x01", "little-endian ELF64"),
            (5, b"\x02", "little-endian ELF64"),
            (8, b"\x03", "ELF OS ABI or ABI version"),
            (16, b"\x02", "wrong ELF type"),
            (18, b"\x3e", "wrong ELF machine"),
            (48, b"\x04", "unsupported RISC-V ELF flags"),
            (40, b"\x08", "invalid section header table"),
            (54, b"\x38", "must not have an entry point or program headers"),
            (64, b"\x01", "invalid null section header"),
            (132, b"\x01", "invalid section name string table"),
            (192, b"\xff", "unterminated section name string table"),
            (128, b"\x20", "invalid section metadata"),
        ):
            with self.subTest(message=message, offset=offset):
                self.obj.write_bytes(baseline[:offset] + replacement + baseline[offset + 1:])
                with self.assertRaisesRegex(ArchonError, rf"link input .*in\.o.*ET_REL.*{message}"):
                    link_executable("ld.lld", [self.obj], "_start", self.dest)
        self.obj.write_bytes(b"!<arch>\n")
        with self.assertRaisesRegex(ArchonError, "truncated ELF header"):
            link_executable("ld.lld", [self.obj], "_start", self.dest)
        with self.assertRaisesRegex(ArchonError, "not a regular file"):
            link_executable("ld.lld", [self.root / "missing.o"], "_start", self.dest)
        invoke.assert_not_called()
        self.assertEqual(self.dest.read_bytes(), b"previous destination")

    @patch("archon.irgen.objectwriter.subprocess.run")
    def test_rejects_option_like_entry_stdout_and_alias_destinations(self, invoke):
        for entry in ("", "0x10000", "@argv", "--help", "_start\n", "\x00_start", "_" * 256):
            with self.subTest(entry=entry), self.assertRaisesRegex(ArchonError, "entry symbol"):
                link_executable("ld.lld", [self.obj], entry, self.dest)
        with self.assertRaisesRegex(ArchonError, "at least one"):
            link_executable("ld.lld", [], "_start", self.dest)
        with self.assertRaisesRegex(ArchonError, "file destination"):
            link_executable("ld.lld", [self.obj], "_start", Path("-"))
        with self.assertRaisesRegex(ArchonError, "must be a regular file"):
            link_executable("ld.lld", [self.obj], "_start", self.root)
        self.dest.unlink()
        self.dest.hardlink_to(self.obj)
        with self.assertRaisesRegex(ArchonError, "must differ"):
            link_executable("ld.lld", [self.obj], "_start", self.dest)
        invoke.assert_not_called()

    @patch("archon.irgen.objectwriter.subprocess.run")
    def test_gnu_linker_is_explicitly_unsupported(self, invoke):
        invoke.return_value = subprocess.CompletedProcess([], 0, stdout=b"GNU ld 2.47\n")
        with self.assertRaisesRegex(ArchonError, "GNU ld is not supported"):
            link_executable("ld", [self.obj], "_start", self.dest)
        self.assertEqual(invoke.call_count, 1)
        self.assertEqual(self.dest.read_bytes(), b"previous destination")

    @patch("archon.irgen.objectwriter.subprocess.run")
    def test_links_private_validated_snapshot(self, invoke):
        original = self.obj.read_bytes()
        header = EHDR.pack(IDENT, 2, 243, 1, 0x10000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
        segment = PHDR.pack(1, 5, 0x1000, 0x10000, 0x10000, 4, 4, 0x1000)
        code = struct.pack("<I", 0x8067)  # jalr x0, 0(ra)

        def produce(argv, **kwargs):
            if argv[1] == "--version":
                self.obj.write_bytes(b"this is no longer the validated object")
                return subprocess.CompletedProcess(argv, 0, stdout=b"LLD 22.1.8\n")
            staged = Path(argv[-1])
            self.assertIn("--no-dependent-libraries", argv)
            self.assertNotEqual(staged, self.obj)
            self.assertEqual(staged.read_bytes(), original)
            Path(argv[argv.index("-o") + 1]).write_bytes((header + segment).ljust(0x1000, b"\0") + code)
            return subprocess.CompletedProcess(argv, 0)

        invoke.side_effect = produce
        link_executable("ld.lld", [self.obj], "_start", self.dest)
        self.assertTrue(self.dest.read_bytes().endswith(code))
        self.assertEqual(set(self.root.iterdir()), {self.obj, self.dest})

    @patch("archon.irgen.objectwriter.subprocess.run")
    def test_malformed_executables_are_not_published(self, invoke):
        # Pretend the linker exited successfully; check the staged artifact
        # independently of the tool exit status.
        valid_header = [IDENT, 2, 243, 1, 0x10000, 64, 0, 0, 64, 56, 1, 0, 0, 0]
        valid_segment = [1, 5, 0x1000, 0x10000, 0x10000, 4, 8, 0x1000]
        for is_header, field, value, reason in (
            (True, 1, 3, "wrong ELF type"),
            (True, 2, 62, "wrong ELF machine"),
            (True, 7, 1, "unsupported RISC-V ELF flags"),
            (True, 10, 100, "invalid program header table"),
            (True, 4, 0x10002, "4-byte aligned"),
            (True, 4, 0x10004, "file-backed executable"),
            (False, 0, 2, "dynamic ELF"),
            (False, 0, 3, "dynamic ELF"),
            (False, 0, 0x6474e551, "executable stacks"),
            (False, 1, 4, "file-backed executable"),
            (False, 1, 7, "writable executable load segments"),
            (False, 2, 0x1004, "load segment geometry"),
            (False, 5, 9, "load segment geometry"),
            (False, 6, 256 * 1024 * 1024 + 1, "exceed memory size limit"),
            (False, 7, 3, "load segment geometry"),
        ):
            with self.subTest(kind=is_header, field=field, value=value):
                header, segment = valid_header.copy(), valid_segment.copy()
                (header if is_header else segment)[field] = value
                image = (EHDR.pack(*header) + PHDR.pack(*segment)).ljust(0x1004, b"\x00")

                def produce(argv, **kwargs):
                    if argv[1] == "--version":
                        return subprocess.CompletedProcess(argv, 0, stdout=b"LLD 22.1.8\n")
                    Path(argv[argv.index("-o") + 1]).write_bytes(image)
                    return subprocess.CompletedProcess(argv, 0)

                invoke.side_effect = produce
                with self.assertRaisesRegex(ArchonError, "linker output .*ET_EXEC.*" + reason):
                    link_executable("ld.lld", [self.obj], "_start", self.dest)
                self.assertEqual(self.dest.read_bytes(), b"previous destination")
                self.assertEqual(set(self.root.iterdir()), {self.obj, self.dest})


class NativeLinkIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        clang, cls.lld = shutil.which("clang"), shutil.which("ld.lld")
        cmake = shutil.which("cmake")
        if not (clang and cls.lld and cmake and (shutil.which("cc") or clang)):
            raise unittest.SkipTest("Clang, LLD, and CMake are required")
        if not re.search(rb"^\s*riscv64\s+-", run(clang, "--print-targets"), re.M):
            raise unittest.SkipTest("Clang lacks RISC-V 64 support")

        work = temporary_directory("archon-native-link-")
        cls.addClassCleanup(work.cleanup)
        cls.root = Path(work.name)
        generated = write_generated_directory(
            generate_ir("gated", [compile_file(SPEC)], options={"profile": "native", "oracle": False}),
            cls.root / "generated",
        )
        build = cls.root / "build"
        run(cmake, "-S", generated, "-B", build)
        run(cmake, "--build", build, "--target", "archon-backend")
        cls.main = cls.root / "@main ; program.o"
        cls.adjust = cls.root / "-adjust.o"
        for source, obj in ((MAIN, cls.main), (ADJUST, cls.adjust)):
            mir = obj.with_suffix(".mir")
            mir.write_text(source)
            assemble_object(build / "archon-backend", clang, "clang", mir, obj)

        target = cls.root / "target.c"
        target.write_text(TARGET)
        cls.start, cls.target = cls.root / "start.o", target.with_suffix(".o")
        for source, obj in ((START, cls.start), (target, cls.target)):
            run(clang, "--target=riscv64-linux-gnu", "-march=rv64im", "-mabi=lp64",
                "-ffreestanding", "-fno-pic", "-fno-pie", "-fno-stack-protector",
                "-O2", "-c", source, "-o", obj)
        cls.objects = (cls.start, cls.target, cls.main, cls.adjust)
        cls.program = cls.root / "static program.elf"
        cls.program.write_bytes(b"previous destination")
        link_executable(cls.lld, cls.objects, "_start", cls.program)

    def test_symbol_addresses_and_patched_relocations(self):
        data = self.program.read_bytes()
        ident, kind, machine, _, entry, _, shoff, flags, _, _, _, shsize, shnum, _ = EHDR.unpack_from(data)
        self.assertEqual((ident[:7], kind, machine, flags), (IDENT, 2, 243, 0))
        self.assertTrue(os.access(self.program, os.X_OK))
        sections = [SHDR.unpack_from(data, shoff + i * shsize) for i in range(shnum)]
        _, _, _, _, symoff, symsize, link, _, _, _ = next(s for s in sections if s[1] == 2)
        strtab = sections[link][4]
        symbols = {data[strtab + name:data.index(0, strtab + name)]: (ndx, addr, sz)
                   for name, _, _, ndx, addr, sz in SYM.iter_unpack(data[symoff:symoff + symsize])
                   if name and ndx}
        self.assertEqual(entry, symbols[b"_start"][1])

        def mapped(name):
            ndx, addr, size = symbols[name]
            section = sections[ndx]
            offset = section[4] + addr - section[3]
            return addr, data[offset:offset + size]

        def signed(value, bits):
            return (value ^ (1 << (bits - 1))) - (1 << (bits - 1))

        def pcrel_pair_targets(name, second_opcode):
            addr, code = mapped(name)
            words = [row[0] for row in struct.iter_unpack("<I", code)]
            return {addr + i * 4 + signed(first & 0xfffff000, 32) + signed(second >> 20, 12)
                    for i, (first, second) in enumerate(zip(words, words[1:]))
                    if first & 0x7f == 0x17 and second & 0x7f == second_opcode
                    and (first >> 7) & 31 == (second >> 15) & 31}

        self.assertIn(symbols[b"target_main"][1], pcrel_pair_targets(b"_start", 0x67))  # jalr
        self.assertIn(symbols[b"archon_adjust"][1], pcrel_pair_targets(b"archon_main", 0x67))
        self.assertIn(symbols[b"native_add2"][1], pcrel_pair_targets(b"archon_adjust", 0x67))
        self.assertIn(symbols[b"datum"][1], pcrel_pair_targets(b"archon_main", 0x13))  # addi
        self.assertEqual(mapped(b"datum")[1], struct.pack("<Q", 40))

    def test_missing_entry_or_import_leaves_no_partial_file(self):
        dest = self.root / "failed.elf"
        with self.assertRaisesRegex(ArchonError, "native linker failed"):
            link_executable(self.lld, self.objects, "missing_entry", dest)
        self.assertFalse(dest.exists())
        dest.write_bytes(b"original")
        with self.assertRaisesRegex(ArchonError, "native linker failed"):
            link_executable(self.lld, self.objects[:-1], "_start", dest)
        self.assertEqual(dest.read_bytes(), b"original")

    def test_unicorn_execution(self):
        try:
            import unicorn
            from unicorn import riscv_const
        except ImportError:
            self.skipTest("execution requires Unicorn with RISC-V support")
        if not unicorn.uc_arch_supported(unicorn.UC_ARCH_RISCV):
            self.skipTest("installed Unicorn lacks RISC-V support")
        from test_irgen_native import unicorn_exit_status

        self.assertEqual(unicorn_exit_status(self.program, unicorn, riscv_const), 0)

    def test_optional_archbfd_inspection(self):
        inspect = os.environ.get("ARCHON_QBFD_INSPECT")
        if not inspect:
            self.skipTest("set ARCHON_QBFD_INSPECT to Yellowstone's archbfd-inspect executable")
        obj = run(inspect, self.main)
        exe = run(inspect, self.program)
        for report in (obj, exe):
            self.assertRegex(report, rb"(?i): elf riscv64\b")
            self.assertIn(b"symbol archon_main", report)
            self.assertIn(b"symbol datum", report)
        self.assertNotIn(b"symbol archon_adjust", obj)
        self.assertIn(b"symbol archon_adjust", exe)
        self.assertIn(b"symbol _start", exe)
