"""Exercise table-derived RV64 encoding and local layout without an assembler."""

from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory


ROOT = Path(__file__).resolve().parents[1]
PLATFORM = ("--target=riscv64-linux-gnu", "-march=rv64im", "-mabi=lp64")
ELF = struct.Struct("<16sHHIQQQIHHHHHH")
SEGMENT = struct.Struct("<IIQQQQQQ")

FACTORIAL = (ROOT / "examples/native-backend/factorial.mir").read_text(encoding="ascii")
INVENTORY = """
archon-mir 1
module 1 0 0
function all_forms void 3 24 3 0
types i64 i64 ptr i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 func_ptr i64 i64
block 0 0
params
cbr 0 1 0 2 0
block 0 {count}
params
add 3 0 1
sub 4 1 0
mul 5 3 4
div_s 6 5 3
rem_s 7 5 3
div_u 8 5 3
rem_u 9 5 3
and 10 6 7
or 11 8 9
xor 12 10 11
shl 13 10 11
lshr 14 10 11
ashr 15 10 11
slt 16 13 14
ult 17 15 16
eq 18 16 17
load 19 2 0xfffffffffffff800
sext 20 19 1
func_addr 21 all_forms
const 22 0xfedcba9876543210
add_checked_s 23 20 22
{stores}call_indirect - 21 all_forms 3 0 1 2
ret -
block 0 0
params
trap
end
""".format(count=22 + 520, stores="store 2 23 2040\n" * 520)

FAULT_INJECTION = r"""
#include "NativeInternal.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag };
    ANOut out = {0};
    uint32_t words[2];

    /* Actual byte displacements are four times these instruction indices. */
    assert(an_encode_pcrel(0, 511, AN_T3, AN_ZERO, AN_JALR, words));
    assert(words[0] >> 12 == 0 && words[1] >> 20 == 2044);
    assert(an_encode_pcrel(0, 512, AN_T3, AN_ZERO, AN_JALR, words));
    assert(words[0] >> 12 == 1 && words[1] >> 20 == 2048);
    assert(an_encode_pcrel(513, 0, AN_T3, AN_ZERO, AN_JALR, words));
    assert(words[0] >> 12 == 0xfffff && words[1] >> 20 == 2044);
    assert(an_encode_pcrel(0, (((size_t)1 << 31) - 2052) / 4, AN_T3, AN_ZERO, AN_JALR, words));
    assert(!an_encode_pcrel(0, (((size_t)1 << 31) - 2048) / 4, AN_T3, AN_ZERO, AN_JALR, words));
    assert(an_encode_pcrel((((size_t)1 << 31) + 2048) / 4, 0, AN_T3, AN_ZERO, AN_JALR, words));
    assert(words[0] >> 12 == 0x80000 && words[1] >> 20 == 0x800);
    assert(!an_encode_pcrel((((size_t)1 << 31) + 2052) / 4, 0, AN_T3, AN_ZERO, AN_JALR, words));
    assert(!an_encode(AN_ADD, 32, 1, 1, 0, words));
    assert(!an_encode(AN_LD, 1, 2, 3, 0, words));
    assert(!an_encode(AN_SLLI, 1, 2, 0, 64, words));
    assert(!an_encode(AN_SD, 1, 2, 0, -2049, words));
    assert(!an_encode(AN_BEQ, 1, 2, 0, 3, words));
    assert(!an_encode(AN_BNE, 1, 2, 0, 4096, words));
    assert(an_encode(AN_BNE, 1, 2, 0, -4096, words));

    assert(an_layout_begin(&out, 0, "f"));
    assert(an_layout_label(&out, 0, 0));
    assert(!an_layout_label(&out, 0, 0));
    assert(out.status == ARCHON_BACKEND_INTERNAL && !strcmp(out.detail, "duplicate native label"));
    an_layout_dispose(&out);

    memset(&out, 0, sizeof out);
    assert(an_layout_begin(&out, 0, "f"));
    assert(an_layout_label(&out, 0, 0));
    assert(an_layout_reference(&out, 0, 1, 0));
    out.instructions = 2;
    assert(!an_layout_finish(&ctx, &out, 1));
    assert(out.status == ARCHON_BACKEND_INTERNAL && strstr(out.detail, "unresolved"));
    an_layout_dispose(&out);

    memset(&out, 0, sizeof out);
    assert(an_layout_begin(&out, 0, "f"));
    assert(an_layout_label(&out, 0, 0));
    assert(an_layout_reference(&out, 0, 0, 0));
    assert(!an_layout_reference(&out, 0, 0, 1));
    assert(out.status == ARCHON_BACKEND_INTERNAL && strstr(out.detail, "overlapping"));
    an_layout_dispose(&out);

    memset(&out, 0, sizeof out);
    assert(an_layout_begin(&out, 0, "f"));
    assert(an_layout_label(&out, 0, 0));
    out.instructions = 1;
    assert(an_layout_label(&out, 0, 1));
    assert(!an_layout_finish(&ctx, &out, 2));
    assert(out.status == ARCHON_BACKEND_INTERNAL && strstr(out.detail, "does not name"));
    an_layout_dispose(&out);

    {
        char code[8] = {0x13}; /* The emitter must reserve zero bytes for a fixup. */
        memset(&out, 0, sizeof out);
        out.machine_code = 1;
        out.data = code;
        out.size = sizeof code;
        assert(an_layout_begin(&out, 0, "f"));
        assert(an_layout_label(&out, 0, 0));
        assert(an_layout_reference(&out, 0, 0, 0));
        out.instructions = 2;
        assert(!an_layout_finish(&ctx, &out, 1));
        assert(out.status == ARCHON_BACKEND_INTERNAL && strstr(out.detail, "would overwrite code"));
        assert(code[0] == 0x13 && code[4] == 0);
        an_layout_dispose(&out);
    }
    return 0;
}
"""


class NativeMachineCodeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc, cmake = shutil.which("cc"), shutil.which("cmake")
        if not cls.cc or not cmake:
            raise unittest.SkipTest("C compiler and CMake required")
        tmp = Path("/tmp/opencode")
        work = tempfile.TemporaryDirectory(dir=tmp if tmp.is_dir() else None)
        cls.addClassCleanup(work.cleanup)
        cls.root = Path(work.name)
        cls.generated, build = cls.root / "generated", cls.root / "build"
        db = compile_file(ROOT / "stdspec/riscv/rv64im.arch")
        write_generated_directory(
            generate_ir("gated", [db], options={"profile": "native", "oracle": False}), cls.generated,
        )
        cls.run_ok(cmake, "-S", cls.generated, "-B", build,
                   "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic-errors")
        cls.run_ok(cmake, "--build", build, "--target", "archon-backend")
        cls.driver, cls.library = build / "archon-backend", build / "libarchon_backend.a"

    @staticmethod
    def run_ok(*argv, text=None):
        result = subprocess.run(argv, input=text, capture_output=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        return result.stdout

    def test_binary_matches_independent_assembler_and_linker(self):
        clang, lld = shutil.which("clang"), shutil.which("ld.lld")
        if not clang or not lld or b"riscv64" not in self.run_ok(clang, "--print-targets"):
            self.skipTest("RISC-V Clang and LLD required")
        for name, program in (("factorial", FACTORIAL), ("all_forms", INVENTORY)):
            for flags in ((), ("--schedule-pure",)):
                with self.subTest(name=name, flags=flags):
                    data = program.encode("ascii")
                    raw = self.run_ok(self.driver, "--input", "-", "--emit-code", *flags, text=data)
                    self.assertTrue(raw and not len(raw) % 4)
                    if name == "all_forms":
                        self.assertGreater(len(raw), 4096)
                    asm, obj, exe = (self.root / f"{name}.{ext}" for ext in ("s", "o", "elf"))
                    asm.write_bytes(self.run_ok(self.driver, "--input", "-", *flags, text=data))
                    self.run_ok(clang, *PLATFORM, "-c", asm, "-o", obj)
                    self.run_ok(lld, "-m", "elf64lriscv", "-static", "--no-relax", "-e", name, obj, "-o", exe)
                    binary = exe.read_bytes()
                    _, _, _, _, entry, phoff, _, _, _, phsize, phnum, *_ = ELF.unpack_from(binary)
                    for i in range(phnum):
                        kind, perms, offset, start, _, size, _, _ = SEGMENT.unpack_from(binary, phoff + i * phsize)
                        if kind == 1 and perms & 1 and start <= entry < start + size:
                            self.assertEqual(entry + len(raw), start + size)
                            self.assertEqual(raw, binary[offset + entry - start:offset + size])
                            break
                    else:
                        self.fail("entry code not found in linked reference")

    def test_execute_raw_relocatable_bytes(self):
        try:
            import unicorn as uc
            from unicorn import riscv_const as reg
        except ImportError:
            self.skipTest("Unicorn with RISC-V support required")
        if not uc.uc_arch_supported(uc.UC_ARCH_RISCV):
            self.skipTest("Unicorn lacks RISC-V support")
        raw = self.run_ok(self.driver, "--input", "-", "--emit-code", text=FACTORIAL.encode("ascii"))
        for address, n, expected in ((0x10000, 7, 5040), (0x20000, 0, 1), (0x30000, 8, 40320)):
            with self.subTest(address=address, n=n):
                machine = uc.Uc(uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV64)
                machine.mem_map(address, 0x1000, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
                machine.mem_write(address, raw)
                machine.mem_protect(address, 0x1000, uc.UC_PROT_READ | uc.UC_PROT_EXEC)
                stack_base = 0x400000
                stack_top = stack_base + 0x4000
                machine.mem_map(stack_base, 0x4000, uc.UC_PROT_READ | uc.UC_PROT_WRITE)
                sentinel = address + 0xffc
                machine.reg_write(reg.UC_RISCV_REG_A0, n)
                machine.reg_write(reg.UC_RISCV_REG_SP, stack_top)
                machine.reg_write(reg.UC_RISCV_REG_RA, sentinel)
                machine.emu_start(address, sentinel, count=10000)
                self.assertEqual(machine.reg_read(reg.UC_RISCV_REG_PC), sentinel)
                self.assertEqual(machine.reg_read(reg.UC_RISCV_REG_A0), expected)
                self.assertEqual(machine.reg_read(reg.UC_RISCV_REG_SP), stack_top)

    def test_local_layout_and_encoding_reject_invalid_records(self):
        client, exe = self.root / "inject.c", self.root / "inject"
        client.write_text(FAULT_INJECTION, encoding="ascii")
        self.run_ok(self.cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic-errors",
                    "-I", self.generated, client, self.library, "-o", exe)
        self.run_ok(exe)

    def test_raw_profile_and_output_mode_errors_are_atomic(self):
        message = b"requires exactly one function, no imports or globals"
        with_import = FACTORIAL.replace("module 1 0 0", "module 1 1 0").replace("end\n", "import ext void 0\nend\n")
        with_global = FACTORIAL.replace("module 1 0 0", "module 1 0 1").replace("end\n", "global ext 1 zero\nend\n")
        with_extra_function = FACTORIAL.replace("module 1 0 0", "module 2 0 0").replace(
            "end\n", "function empty void 0 0 1 0\ntypes\nblock 0 0\nparams\nret -\nend\n",
        )
        for data, extra, expected in (
            (with_import, [], message),
            (with_global, [], message),
            (with_extra_function, [], message),
            (FACTORIAL, ["--emit-physical"], b"choose one output mode"),
        ):
            with self.subTest(module=data.splitlines()[1], extra=extra):
                result = subprocess.run([self.driver, "--input", "-", "--emit-code", *extra],
                                        input=data.encode("ascii"), capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr)
                self.assertFalse(result.stdout)
