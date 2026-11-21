"""Build and independently check the generated RV64I/LP64 backend."""

import json
import os
from pathlib import Path
from random import Random
import shutil
import struct
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen import generate_ir, write_generated_directory
from archon.irgen.objectwriter import assemble_object


ROOT = Path(__file__).resolve().parents[1]
RV64I = ROOT / "stdspec/riscv/rv64i.arch"
FLAGS = ("--target=riscv64-linux-gnu", "-march=rv64i", "-mabi=lp64")
MIR = b"""\
archon-mir 1
module 1 0 0
function product i64 2 3 1 0
types i64 i64 i64
block 0 1
params
mul 2 0 1
ret 2
end
"""
ABI = b"""\
archon-mir 1
module 1 1 1
function abi_probe i64 1 19 1 1
types i64 ptr i64 func_ptr ptr func_ptr i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
local 16 8
block 0 20
params
global_addr 1 external_data
load_s8 2 1 0
func_addr 3 foreign_ten
frame_addr 4 0
store_func_ptr 4 3 8
load_func_ptr 5 4 8
const 6 1
const 7 2
const 8 3
const 9 4
const 10 5
const 11 6
const 12 7
const 13 8
const 14 9
call_indirect 15 5 foreign_ten 10 0 6 7 8 9 10 11 12 13 14
store8 4 0 1
load_u8 16 4 1
add 17 15 2
add 18 17 16
ret 18
import foreign_ten i64 10 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
global external_data 0 zero
end
"""
START = """\
.text
.globl _start
_start:
    call test_main
    li a7, 93
    ecall
"""


class NativeRV64ITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cmake = shutil.which("cmake")
        if not cmake or not shutil.which("cc"):
            raise unittest.SkipTest("C99 compiler and CMake required")
        work = tempfile.TemporaryDirectory(prefix="archon-rv64i-")
        cls.addClassCleanup(work.cleanup)
        cls.root = Path(work.name)
        cls.db = compile_file(RV64I)
        cls.files = generate_ir("gated", [cls.db], options={"profile": "native", "oracle": False})
        generated = write_generated_directory(cls.files, cls.root / "generated")
        build = cls.root / "build"
        cls.run_ok(cmake, "-S", generated, "-B", build,
                   "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic-errors")
        cls.run_ok(cmake, "--build", build)
        cls.driver = build / "archon-backend"

    @staticmethod
    def run_ok(*argv, data=None):
        result = subprocess.run(argv, input=data, capture_output=True, timeout=60)
        if result.returncode:
            raise AssertionError((argv, result.returncode, result.stdout, result.stderr))
        return result.stdout

    def tool(self, name):
        path = shutil.which(name)
        if not path:
            self.skipTest(f"{name} is required")
        return path

    def clang(self):
        clang = self.tool("clang")
        if b"riscv64" not in self.run_ok(clang, "--print-targets"):
            self.skipTest("Clang lacks RV64 support")
        return clang

    def test_factir_fallthrough_preserves_phi_copies_and_other_modes(self):
        mir = b"""\
archon-mir 1
module 1 0 0
function choose i64 3 4 3 0
types i64 i64 i64 i64
block 0 0
params
cbr 0 1 1 1 2 0
block 1 0
params 3
ret 3
block 0 0
params
ret 2
end
"""
        source, driver = self.root / "edge_facts.c", self.root / "edge_facts"
        source.write_text(r"""
#include "Backend.h"
#include <stdio.h>
#include <stdlib.h>

static int contains(const void *user, ArchonBackendFactKind kind,
                    size_t function, size_t block, uint32_t target)
{
    return kind == ARCHON_BACKEND_FACT_CONTROL_EDGE && function == 0 && block == 0 &&
        target == *(const unsigned *)user;
}

int main(int argc, char **argv)
{
    char text[4096];
    size_t size = fread(text, 1, sizeof(text), stdin);
    ArchonBackendArtifact artifact = {0};
    unsigned target, flags;
    if (argc != 3 || !size || size == sizeof(text)) return 1;
    target = (unsigned)strtoul(argv[1], NULL, 10);
    flags = (unsigned)strtoul(argv[2], NULL, 10);
    const ArchonBackendFactHints facts = {contains, NULL, &target};
    ArchonBackendStatus status = archon_backend_compile_text_with_facts(
        text, size, NULL, flags, target < 4 ? &facts : NULL, &artifact, NULL);
    if (status != ARCHON_BACKEND_OK)
        return artifact.data || artifact.size ? 99 : (int)status;
    fwrite(artifact.data, 1, artifact.size, stdout);
    archon_backend_artifact_destroy(&artifact);
    return 0;
}
""", encoding="ascii")
        self.run_ok("cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-I", self.root / "generated", source,
                    self.driver.parent / "libarchon_backend.a", "-o", driver)
        baseline = self.run_ok(self.driver, "--input", "-", data=mir)
        for target in ("2", "3", "4"):
            self.assertEqual(baseline, self.run_ok(driver, target, "0", data=mir))
        optimized = self.run_ok(driver, "1", "0", data=mir)
        jump = (b"2:\n\tauipc x28, %pcrel_hi(.Larchon0_1)\n"
                b"\tjalr x0, %pcrel_lo(2b)(x28)\n")
        self.assertEqual(baseline.count(jump), 1)
        self.assertEqual(optimized, baseline.replace(jump, b""))
        for flags in ("2", "4"):  # Physical records and raw machine code.
            self.assertEqual(self.run_ok(driver, "1", flags, data=mir),
                             self.run_ok(driver, "4", flags, data=mir))
        result = subprocess.run([driver, "1", "0"],
                                input=mir.replace(b"cbr 0 1 1 1", b"cbr 0 1 0"),
                                capture_output=True, timeout=10)
        self.assertEqual((result.returncode, result.stdout), (1, b""))
        qemu = (os.environ.get("ARCHON_QEMU_RISCV64") or shutil.which("qemu-riscv64")
                or shutil.which("qemu-riscv64-static"))
        if qemu and shutil.which("clang") and (lld := shutil.which("ld.lld")):
            asm, harness, output = (self.root / name for name in
                                    ("edge_run.s", "edge_run.c", "edge_run.elf"))
            harness.write_text("""\
extern unsigned long choose(unsigned long, unsigned long, unsigned long);
int test_main(void) {
    return choose(0, 17, 42) != 42 || choose(~0UL, 17, 42) != 17;
}
""", encoding="ascii")
            for code in (baseline, optimized):
                asm.write_bytes(code + START.encode("ascii"))
                self.run_ok(self.clang(), *FLAGS, "-fuse-ld=" + lld, "-nostdlib", "-static",
                            "-ffreestanding", "-fno-stack-protector",
                            "-Wl,--no-relax,-e,_start", asm, harness, "-o", output)
                self.run_ok(qemu, "-cpu", "rv64,m=false", output)

    def test_provider_manifest_and_target_identity(self):
        manifest = json.loads(self.files["BackendManifest.json"])
        self.assertEqual((manifest["target"], manifest["isa"], manifest["abi"]),
                         ("RISCV64", "RV64I", "LP64"))
        self.assertEqual(manifest["isa_features"], ["I"])
        self.assertEqual(manifest["floating_point"]["formats"], [])
        self.assertIn("mul", manifest["expanded_mir"])
        self.assertEqual(set(manifest["unsupported_mir"]),
                         {"div_s", "div_u", "rem_s", "rem_u", "div_checked_s", "mul_checked_s"})
        for stem in ("MUL", "DIV", "REM", "DIVU", "REMU"):
            self.assertNotIn(stem, manifest["instruction_evidence"])
            self.assertNotIn(f"[AN_{stem}]", self.files["NativeEncodingData.c"])
        records = self.run_ok(self.driver, "--input", "-", "--emit-physical", data=MIR)
        self.assertEqual(records.splitlines()[:2], [b"archon-physical 1", b"target RISCV64/LP64"])
        self.assertIn(b"inst slli", records)
        self.assertNotIn(b"inst mul", records)
        with self.assertRaisesRegex(ArchonError, "does not support the requested native ABI"):
            generate_ir("gated", [self.db],
                        options={"profile": "native", "abi": "ILP32", "oracle": False})

    def test_unsupported_operations_are_input_errors_for_every_sink(self):
        for opcode in (b"div_s", b"div_u", b"rem_s", b"rem_u", b"div_checked_s", b"mul_checked_s"):
            for verb, flags in (("--verify-input", ()), ("--input", ()),
                                ("--input", ("--emit-code",)), ("--input", ("--emit-physical",))):
                with self.subTest(opcode=opcode, verb=verb, flags=flags):
                    result = subprocess.run([self.driver, verb, "-", *flags],
                                            input=MIR.replace(b"mul", opcode),
                                            capture_output=True, timeout=10)
                    self.assertEqual((result.returncode, result.stdout), (1, b""))
                    self.assertIn(b"function 0, block 0, instruction 0", result.stderr)
                    self.assertIn(opcode + b" is unsupported without M (RV64I)", result.stderr)

    def test_known_bits_select_base_shift_without_overriding_legality(self):
        source = self.root / "fact_select.c"
        driver = self.root / "fact_select"
        source.write_text(r"""
#include "Backend.h"
#include <stdio.h>
#include <string.h>

static int known(const void *user, size_t function, ArchonValue value,
                 uint64_t *zero, uint64_t *one)
{
    const char *mode = user;
    if (function || value != 1 || strcmp(mode, "absent") == 0)
        return 0;
    *one = 8;
    *zero = strcmp(mode, "partial") == 0 ? 0 :
            strcmp(mode, "conflict") == 0 ? UINT64_MAX : ~UINT64_C(8);
    return 1;
}

int main(int argc, char **argv)
{
    char text[1024];
    size_t size = fread(text, 1, sizeof(text), stdin);
    ArchonBackendArtifact artifact = {0};
    ArchonBackendDiagnostic diagnostic;
    if (argc != 3 || !size || size == sizeof(text)) return 1;
    const ArchonBackendFactHints facts = {.known_bits = known, .user = argv[1]};
    unsigned flags = strcmp(argv[2], "code") == 0 ? ARCHON_BACKEND_MACHINE_CODE :
                     ARCHON_BACKEND_PHYSICAL_RECORDS;
    if (archon_backend_compile_text_with_facts(text, size, NULL, flags,
            &facts, &artifact, &diagnostic) != ARCHON_BACKEND_OK) {
        fprintf(stderr, "%s\n", diagnostic.message);
        return 2;
    }
    fwrite(artifact.data, 1, artifact.size, stdout);
    archon_backend_artifact_destroy(&artifact);
    return 0;
}
""", encoding="ascii")
        self.run_ok(self.tool("cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-I", self.root / "generated", source,
                    self.root / "build/libarchon_backend.a", "-o", driver)
        mir = MIR.replace(b"i64 2 3 1 0", b"i64 1 3 1 0").replace(
            b"block 0 1\nparams", b"block 0 2\nparams\nconst 1 8")
        for mode in ("records", "code"):
            baseline = self.run_ok(driver, "absent", mode, data=mir)
            for hints in ("partial", "conflict"):
                self.assertEqual(self.run_ok(driver, hints, mode, data=mir), baseline)
            selected = self.run_ok(driver, "exact", mode, data=mir)
            if mode == "records":
                self.assertRegex(selected, rb"(?m)^inst slli [^\n]+ 3$")
                self.assertNotIn(b"inst beq", selected)
            else:
                self.assertLess(len(selected), len(baseline))
                words = [word for (word,) in struct.iter_unpack("<I", selected)]
                self.assertTrue(any(word & 0xfff0707f == 0x00301013 for word in words))
            stale = mir.replace(b"const 1 8", b"const 1 12")
            self.assertEqual(self.run_ok(driver, "exact", mode, data=stale),
                             self.run_ok(driver, "absent", mode, data=stale))
            self.assertEqual(self.run_ok(driver, "exact", mode, data=MIR),
                             self.run_ok(driver, "absent", mode, data=MIR))
            rejected = subprocess.run([driver, "exact", mode],
                                      input=mir.replace(b"mul ", b"mul_checked_s "),
                                      capture_output=True, timeout=10)
            self.assertEqual((rejected.returncode, rejected.stdout), (2, b""))
            self.assertIn(b"mul_checked_s is unsupported without M", rejected.stderr)

    def test_soft_float_abi_does_not_accept_fp_parameters(self):
        floating = b"""archon-mir 1
module 1 0 0
function fp_argument void 1 1 1 0
types f32
block 0 0
params
ret -
end
"""
        result = subprocess.run([self.driver, "--verify-input", "-"],
                                input=floating, capture_output=True, timeout=10)
        self.assertEqual((result.returncode, result.stdout), (1, b""))
        self.assertIn(b"RV64 LP64 is soft-float", result.stderr)

    def test_variadic_prologue_requires_rv64im(self):
        variadic = b"""archon-mir 1
module 1 0 0
function cursor void 1 1 1 0
types ptr
block 0 1
params
rv64_va_start 0
ret -
end
"""
        result = subprocess.run([self.driver, "--verify-input", "-"],
                                input=variadic, capture_output=True, timeout=10)
        self.assertEqual((result.returncode, result.stdout), (1, b""))
        self.assertIn(b"RV64_VA_START requires RV64IM LP64", result.stderr)

    def test_base_bytes_match_linked_rv64i_assembly(self):
        clang, lld, objcopy = self.clang(), self.tool("ld.lld"), self.tool("llvm-objcopy")
        asm, obj, exe, text = (self.root / f"bytes.{ext}" for ext in ("s", "o", "elf", "bin"))
        for flags in ((), ("--schedule-pure",)):
            asm.write_bytes(self.run_ok(self.driver, "--input", "-", *flags, data=MIR))
            self.run_ok(clang, *FLAGS, "-c", asm, "-o", obj)
            self.run_ok(lld, "-m", "elf64lriscv", "-static", "--no-relax",
                        "-e", "product", obj, "-o", exe)
            self.run_ok(objcopy, "--dump-section", f".text={text}", exe)
            raw = self.run_ok(self.driver, "--input", "-", "--emit-code", *flags, data=MIR)
            self.assertEqual(raw, text.read_bytes())
            self.assertTrue(raw)
            for (word,) in struct.iter_unpack("<I", raw):
                self.assertEqual(word & 3, 3)
                # The M OP and OP-32 encodings have funct7 = 1.
                self.assertFalse((word & 0x7f) in (0x33, 0x3b) and word >> 25 == 1)

    def test_assembly_and_raw_code_execute_with_m_disabled(self):
        clang, lld = self.clang(), self.tool("ld.lld")
        qemu = (os.environ.get("ARCHON_QEMU_RISCV64") or shutil.which("qemu-riscv64")
                or self.tool("qemu-riscv64-static"))
        raw, asm, pic, harness, output = (
            self.root / name for name in ("run.bin", "run.s", "pic.s", "run.c", "run.elf")
        )
        raw.write_bytes(self.run_ok(self.driver, "--input", "-", "--emit-code", data=MIR))
        asm.write_bytes(
            self.run_ok(self.driver, "--input", "-", data=MIR)
            + (START + '\n.p2align 2\n.globl raw_product\nraw_product:\n'
                       f'.incbin "{raw}"\n').encode("ascii"),
        )
        pic.write_bytes(self.run_ok(self.driver, "--input", "-", "--pic", data=ABI))
        mask = (1 << 64) - 1
        values = 0, 1, 2, 127, 1 << 32, (1 << 63) - 1, 1 << 63, mask
        rng = Random(64)
        pairs = [(a, b) for a in values for b in values]
        pairs += [(rng.getrandbits(64), rng.getrandbits(64)) for _ in range(128)]
        vectors = "\n".join(f"    {{{a:#x}UL, {b:#x}UL, {a * b & mask:#x}UL}},"
                            for a, b in pairs)
        harness.write_text(
            """\
typedef unsigned long U;
extern U product(U, U), raw_product(U, U), abi_probe(U);
const U external_data = 0x80;
U foreign_ten(U a, U b, U c, U d, U e, U f, U g, U h, U i, U j)
{
    if (b != 1 || c != 2 || d != 3 || e != 4 || f != 5 || g != 6 || h != 7
            || i != 8 || j != 9)
        return 1;
    return a + i + j;
}
static const U samples[][3] = {
""" + vectors + """
};
int test_main(void)
{
    unsigned i;
    for (i = 0; i < sizeof samples / sizeof *samples; ++i) {
        const U *s = samples[i];
        if (product(s[0], s[1]) != s[2])
            return 1;
        if (raw_product(s[0], s[1]) != s[2])
            return 2;
    }
    return abi_probe(0xf0) == 369 ? 0 : 3;
}
""", encoding="ascii")
        self.run_ok(clang, *FLAGS, "-fuse-ld=" + lld, "-nostdlib", "-static",
                    "-ffreestanding", "-fno-stack-protector", "-O2",
                    "-Wl,--no-relax,-e,_start", asm, pic, harness, "-o", output)
        self.run_ok(qemu, "-cpu", "rv64,m=false", output)

    def test_object_bridge_keeps_rv64i_elf_attribute_when_assembler_defaults_to_m(self):
        clang, readelf = self.clang(), self.tool("llvm-readelf")
        source, obj = self.root / "object.mir", self.root / "bridge.o"
        source.write_bytes(MIR)
        assemble_object(self.driver, clang, "clang", source, obj)
        attrs = self.run_ok(readelf, "-A", obj)
        self.assertRegex(attrs, rb"Value: rv64i2p1\s")
        self.assertNotIn(b"_m", attrs)


if __name__ == "__main__":
    unittest.main()
