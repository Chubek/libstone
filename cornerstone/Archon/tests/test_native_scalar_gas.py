"""Independently assemble, link, and execute the AMD64 and AArch64 providers."""

from copy import deepcopy
import json
import os
from pathlib import Path
import platform
from random import Random
import shutil
import signal
import struct
import subprocess
import tempfile
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen import generate_ir, write_generated_directory
from archon.irgen.native_target import resolve_native_target


ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    "amd64": ("AMD64", "SysVAMD64", "amd64-sysv-elf-lp64"),
    "aarch64": ("AArch64", "AAPCS64", "aarch64-aapcs64-elf-lp64"),
}
MASK = (1 << 64) - 1
MIN = 1 << 63
BINARY = ("add sub mul and or xor shl lshr ashr eq slt ult div_s rem_s div_u rem_u "
          "add_checked_s sub_checked_s mul_checked_s div_checked_s").split()


def module(*functions, imports=(), globals_=()):
    return (f"archon-mir 1\nmodule {len(functions)} {len(imports)} {len(globals_)}\n" +
            "\n".join((*functions, *imports, *globals_, "end\n"))).encode("ascii")


def binary(op):
    return f"""\
function mir_{op} i64 2 3 1 0
types i64 i64 i64
block 0 1
params
{op} 2 0 1
ret 2"""


def signed(n):
    return n if n < MIN else n - (1 << 64)


def integer_expected(op, a, b):
    sa, sb = signed(a), signed(b)
    if b:
        sq = (abs(sa) // abs(sb)) * (-1 if (sa < 0) != (sb < 0) else 1)
        uq = a // b
    else:
        sq = uq = -1
    if op.endswith("_checked_s"):
        exact = {"add_checked_s": sa + sb, "sub_checked_s": sa - sb,
                 "mul_checked_s": sa * sb, "div_checked_s": sq}[op]
        if not -MIN <= exact < MIN or (op == "div_checked_s" and not b):
            return None
        return exact & MASK
    return {
        "add": a + b, "sub": a - b, "mul": a * b, "and": a & b, "or": a | b, "xor": a ^ b,
        "shl": a << (b & 63), "lshr": a >> (b & 63), "ashr": sa >> (b & 63),
        "eq": int(a == b), "slt": int(sa < sb), "ult": int(a < b),
        "div_s": sq, "rem_s": sa - sq * sb, "div_u": uq, "rem_u": a - uq * b,
    }[op] & MASK


CFG = """\
function mir_swap_loop i64 3 10 3 0
types i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 2
params
const 3 0
const 4 1
br 1 3 0 1 2
block 3 2
params 5 6 7
slt 8 3 7
sub 9 7 4
cbr 8 1 3 6 5 9 2 0
block 0 0
params
ret 5"""

MEMORY = """\
function mir_memory i64 3 36 1 2
types ptr func_ptr i64 ptr ptr ptr ptr i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 ptr func_ptr i64 i64 i64 ptr func_ptr i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64 i64
local 40000 16
local 64 16
block 0 43
params
frame_addr 3 1
ptr_add 4 3 2
global_addr 5 external_word
global_addr 6 archon_zero
const 7 0xfedcba9887658381
store8 4 7 0xfffffffffffffff8
store16 4 7 26
store32 4 7 28
store 4 7 8
store_ptr 4 0 16
store_func_ptr 4 1 0
load_s8 8 4 0xfffffffffffffff8
load_u8 9 4 0xfffffffffffffff8
load_s16 10 4 26
load_u16 11 4 26
load_s32 12 4 28
load_u32 13 4 28
load 14 4 8
load 15 5 0
load 16 6 0
load_ptr 17 4 16
load_func_ptr 18 4 0
ptr_to_int 19 17
func_ptr_to_int 20 18
ptr_to_int 21 0
int_to_ptr 22 21
int_to_func_ptr 23 20
load 24 22 0
func_ptr_to_int 25 23
eq 26 19 21
eq 27 20 25
add 28 8 9
add 29 10 11
add 30 12 13
xor 31 14 15
add 32 16 24
add 33 28 29
add 34 30 31
add 35 33 34
store 6 35 0
store 5 32 0
store 0 26 8
store 0 27 16
ret 35"""

def call_module():
    argc = 12
    types = ["i64"] * (argc - 1) + ["ptr"]
    all_args = " ".join(map(str, range(argc)))
    rev_args = " ".join(map(str, (*reversed(range(argc - 1)), argc - 1)))
    return module(
        f"""\
function mir_outgoing i64 {argc} 16 1 1
types {' '.join(types)} i64 i64 i64 i64
local 40000 16
block 0 5
params
call 12 native_args {argc} {rev_args}
call - native_void 1 11
call 13 x0 {argc} {all_args}
call 14 stack_status 0
add 15 14 13
ret 15""",
        f"""\
function x0 i64 {argc} 14 1 1
types {' '.join(types)} func_ptr i64
local 40000 16
block 0 2
params
func_addr 12 native_args
call_indirect 13 12 native_args {argc} {all_args}
ret 13""",
        imports=(f"import native_args i64 {argc} {' '.join(types)}",
                 "import native_void void 1 ptr", "import stack_status i64 0"),
    )


def conversion_functions():
    for op in ("trunc", "zext", "sext"):
        for width in (1, 7, 8, 31, 32, 63, 64):
            yield f"""\
function mir_{op}_{width} i64 1 3 1 0
types i64 i64 i64
block 0 2
params
copy 1 0
{op} 2 1 {width}
ret 2"""


START = {
    "amd64": """\
.text
.globl _start
_start:
    call test_main
    movl %eax, %edi
    movl $60, %eax
    syscall
.globl stack_status
stack_status:
    movq %rsp, %rax
    andq $15, %rax
    ret
.section .note.GNU-stack,"",%progbits
""",
    "aarch64": """\
.text
.globl _start
_start:
    bl test_main
    mov x8, #93
    svc #0
.globl stack_status
stack_status:
    mov x0, sp
    and x0, x0, #15
    ret
.section .note.GNU-stack,"",%progbits
""",
}


class ReviewedScalarTargetsTests(unittest.TestCase):
    def test_exact_archdb_and_abi_required(self):
        for arch, (name, abi, provider) in TARGETS.items():
            with self.subTest(arch=arch):
                db = compile_file(ROOT / "stdspec" / arch / "root.arch")
                facts = resolve_native_target(database.loads(database.dumps(db)))
                self.assertEqual((facts["name"], facts["abi"], facts["provider"]),
                                 (name, abi, provider))
                self.assertEqual(facts["abi_facts_v2"], db["abi_facts_v2"])
                facts["abi_facts_v2"]["args"].clear()
                self.assertTrue(resolve_native_target(db)["abi_facts_v2"]["args"])
                for mutate, message in (
                    (lambda d: d.pop("abi_facts_v2"), "missing declarative abi_facts_v2"),
                    (lambda d: d["abi_facts_v2"]["args"].reverse(), "abi_facts_v2.args"),
                    (lambda d: d["abi_facts_v2"].update(stack_align=32), "abi_facts_v2.stack_align"),
                    (lambda d: d["instructions"].pop(), "database fingerprint differs"),
                ):
                    changed = deepcopy(db)
                    mutate(changed)
                    with self.assertRaisesRegex(ArchonError, message):
                        resolve_native_target(changed)
                with self.assertRaisesRegex(ArchonError, "requested native ABI"):
                    generate_ir("gated", [db], options={"profile": "native", "abi": "ILP32"})
                classic = generate_ir("gated", [db], options={"oracle": False})
                self.assertNotIn("BackendManifest.json", classic)


class _ScalarGasTests:
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake") or not shutil.which("cc"):
            raise unittest.SkipTest("requires CMake and a host C compiler")
        temp = Path("/tmp/opencode")
        work = tempfile.TemporaryDirectory(prefix=f"archon-{cls.arch}-",
                                           dir=temp if temp.is_dir() else None)
        cls.addClassCleanup(work.cleanup)
        cls.root = Path(work.name)
        cls.db = compile_file(ROOT / "stdspec" / cls.arch / "root.arch")
        cls.files = generate_ir("gated", [cls.db], options={"profile": "native", "oracle": False})
        cls.generated = write_generated_directory(cls.files, cls.root / "source")
        cls.build = cls.root / "build"
        cls.run_ok("cmake", "-S", cls.generated, "-B", cls.build,
                   "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic-errors")
        cls.run_ok("cmake", "--build", cls.build, "--parallel", "2")
        cls.driver = cls.build / "archon-backend"

    @staticmethod
    def invoke(*argv, data=None):
        return subprocess.run(argv, input=data, capture_output=True, timeout=60)

    @classmethod
    def run_ok(cls, *argv, data=None):
        result = cls.invoke(*argv, data=data)
        if result.returncode:
            raise AssertionError((argv, result.returncode, result.stdout, result.stderr))
        return result.stdout

    def tool(self, name):
        tool = shutil.which(name)
        if not tool:
            self.skipTest(f"requires {name}")
        return tool

    def compiler(self):
        if self.arch == "amd64":
            if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64"):
                self.skipTest("requires a native AMD64 Linux toolchain")
            return [self.tool("cc")]
        clang = self.tool("clang")
        if b"aarch64" not in self.run_ok(clang, "--print-targets"):
            self.skipTest("Clang has no AArch64 target")
        return [clang, "--target=aarch64-linux-gnu", "-march=armv8-a"]

    def executor(self):
        if self.arch == "amd64" or (platform.system() == "Linux" and platform.machine() == "aarch64"):
            return []
        qemu = (os.environ.get("ARCHON_QEMU_AARCH64") or shutil.which("qemu-aarch64")
                or self.tool("qemu-aarch64-static"))
        return [qemu]

    def emit_asm(self, data, *, pic=False):
        flags = ("--pic",) if pic else ()
        return self.run_ok(self.driver, "--input", "-", *flags, data=data)

    def link(self, directory, programs, harness, *, pic=False):
        compiler = self.compiler()
        sources = []
        for i, prog in enumerate(programs):
            path = directory / f"generated{i}.s"
            path.write_bytes(self.emit_asm(prog, pic=pic))
            sources.append(path)
        start, source = directory / "start.s", directory / "harness.c"
        start.write_text(START[self.arch], encoding="ascii")
        source.write_text(harness, encoding="ascii")
        objects = []
        for src in (*sources, start, source):
            obj = src.with_suffix(".o")
            assembler = (["as"] if self.arch == "amd64" else
                         ([cross] if (cross := shutil.which("aarch64-linux-gnu-as")) else None))
            if assembler and src.suffix == ".s":
                self.run_ok(*assembler, "-o", obj, src)
            elif src.suffix == ".s":
                self.run_ok(*compiler, "-c", src, "-o", obj)
            else:
                self.run_ok(*compiler, "-ffreestanding", "-fno-pie", "-fno-stack-protector",
                            "-Wall", "-Wextra", "-Werror", "-O2", "-c", src, "-o", obj)
            objects.append(obj)
        out = directory / "run.elf"
        if self.arch == "amd64":
            self.run_ok(self.tool("ld"), "-static", *objects, "-o", out)
        elif cross_ld := shutil.which("aarch64-linux-gnu-ld"):
            self.run_ok(cross_ld, "-static", *objects, "-o", out)
        else:
            self.run_ok(self.tool("ld.lld"), "-m", "aarch64elf", "-static", *objects, "-o", out)
        header = struct.unpack("<16sHH", out.read_bytes()[:20])
        self.assertEqual((header[0][:6], header[1], header[2]),
                         (b"\x7fELF\x02\x01", 2, 62 if self.arch == "amd64" else 183))
        return out

    def test_capabilities_and_atomic_failures(self):
        target, abi, provider = TARGETS[self.arch]
        manifest = json.loads(self.files["BackendManifest.json"])
        self.assertEqual((manifest["target"], manifest["abi"], manifest["provider"]),
                         (target, abi, provider))
        self.assertEqual(manifest["abi_facts_v2"], self.db["abi_facts_v2"])
        self.assertEqual(manifest["artifact"], ["ELF-oriented GAS assembly (default or PIC)"])
        self.assertEqual(self.run_ok(self.driver, "--list-capabilities"),
                         (f"{target}/{abi}: canonical-mir-v1 -> "
                           "ELF GAS assembly only (default or PIC)\n").encode())
        self.assertEqual(self.run_ok(self.driver, "--verify-input", "-", data=module(CFG)), b"")
        for flag in ("--emit-code", "--emit-physical", "--schedule-pure"):
            result = self.invoke(self.driver, "--input", "-", flag, data=module(CFG))
            self.assertEqual((result.returncode, result.stdout), (1, b""))
            self.assertIn(b"assembly in input order", result.stderr)
        large = """\
function huge void 0 0 1 1
types
local 4294967295 16
block 0 0
params
ret -"""
        res = self.invoke(self.driver, "--input", "-", data=module(CFG, large))
        self.assertEqual((res.returncode, res.stdout), (1, b""))
        self.assertIn(b"64 KiB", res.stderr)
        malformed = binary("add").replace("add 2 0 1", "add 2 2 1")
        res = self.invoke(self.driver, "--input", "-", data=module(CFG, malformed))
        self.assertEqual((res.returncode, res.stdout), (1, b""))
        self.assertIn(b"definition does not dominate use", res.stderr)

    def test_in_memory_runtime_abi(self):
        source, out = self.root / "client.c", self.root / "client"
        source.write_text(r"""
#include "Backend.h"
#include <assert.h>
#include <string.h>
static int cancelled(void *unused) { (void)unused; return 1; }
int main(void)
{
    static const ArchonMIRType ts[] = { ARCHON_MIR_I64 };
    static const ArchonMIRInst cs[] = {
        {ARCHON_MIR_CONST, 0, ARCHON_MIR_NONE, ARCHON_MIR_NONE, UINT64_MAX, 0, 0, 0}
    };
    static const ArchonMIRBlock bs[] = {
        {0, 0, cs, 1, {ARCHON_MIR_RETURN, 0, {0, 0, 0}, {0, 0, 0}}}
    };
    static const ArchonMIRFunction fs[] = {
        {"probe", ARCHON_MIR_I64, ts, 1, 0, bs, 1, 0, 0}
    };
    static const ArchonMIRModule m = { ARCHON_MIR_SCHEMA_VERSION, fs, 1, 0, 0, 0, 0 };
    ArchonBackendArtifact a = {0};
    ArchonBackendDiagnostic d;
    ArchonBackendOptions o = {1, 0, 0};
    assert(ARCHON_BACKEND_ABI_VERSION == 1u);
    assert(archon_backend_compile(&m, 0, &a, &d) == ARCHON_BACKEND_OK);
    assert(a.data && a.size == strlen(a.data) && strstr(a.data, "probe"));
    archon_backend_artifact_destroy(&a);
    assert(archon_backend_compile(&m, &o, &a, &d) == ARCHON_BACKEND_LIMIT);
    assert(d.stage == ARCHON_BACKEND_EMIT && !a.data && !a.size);
    assert(archon_backend_compile_physical(&m, 0, &a, &d) == ARCHON_BACKEND_UNSUPPORTED);
    assert(!a.data && !a.size);
    o.cancelled = cancelled;
    assert(archon_backend_compile(&m, &o, &a, &d) == ARCHON_BACKEND_CANCELLED);
    assert(!a.data && !a.size);
}
""", encoding="ascii")
        self.run_ok("cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-I", self.generated, source, self.build / "libarchon_backend.a", "-o", out)
        self.run_ok(out)

    def test_arithmetic_calls_control_flow_and_memory(self):
        values = (0, 1, 2, 63, 64, 127, (1 << 32) + 31, MIN - 1, MIN, MASK - 2, MASK)
        rng = Random(2000)
        pairs = [(a, b) for a in values for b in values]
        pairs += [(rng.getrandbits(64), rng.getrandbits(64)) for _ in range(96)]
        rows = []
        for op in BINARY:
            for a, b in pairs:
                if (expect := integer_expected(op, a, b)) is not None:
                    rows.append(f"{{mir_{op}, {a:#x}UL, {b:#x}UL, {expect:#x}UL}},")
        conversions = []
        mode = (("trunc", False), ("zext", False), ("sext", True))
        widths = (1, 7, 8, 31, 32, 63, 64)
        for op, extend in mode:
            for width in widths:
                for value in values:
                    expect = value & ((1 << width) - 1)
                    if extend and expect >> (width - 1):
                        expect -= 1 << width
                    conversions.append(f"{{mir_{op}_{width}, {value:#x}UL, {expect & MASK:#x}UL}},")
        directory = self.root / "success"
        directory.mkdir()
        out = self.link(directory, (
            module(*map(binary, BINARY), CFG, *conversion_functions()),
            module(MEMORY, globals_=("global external_word 0 zero", "global archon_zero 1 zero")),
            call_module(),
            module(globals_=("global archon_initialized 1 init 0xabcdef9876543210",)),
        ), "\n".join((
            "typedef unsigned long U;",
            *(f"extern U mir_{op}(U,U);" for op in BINARY),
            *(f"extern U mir_{op}_{width}(U);" for op, _ in mode for width in widths),
            "static const struct { U (*fn)(U,U); U a,b,want; } pairs[] = {",
            *rows, "};",
            "static const struct { U (*fn)(U); U a,want; } widths[] = {",
            *conversions, "};",
            f"#define STACK_REMAINDER {8 if self.arch == 'amd64' else 0}",
            r"""
extern U mir_swap_loop(U,U,U);
extern U mir_memory(U *, U (*)(U,U), U);
extern U mir_outgoing(U,U,U,U,U,U,U,U,U,U,U,U *);
extern U archon_zero, archon_initialized;
U external_word = 1UL << 32;
static U last, calls;
void native_void(U *p) { *p += 7; }
U native_args(U a,U b,U c,U d,U e,U f,U g,U h,U i,U j,U k,U *p)
{
    U args[] = {a,b,c,d,e,f,g,h,i,j,k};
    U r = *p;
    for (unsigned t = 0; t < 11; ++t) {
        if (args[t] != (calls ? t+1 : 11-t)) return ~0UL;
        r += args[t] * (t+3);
    }
    ++calls;
    last = r;
    return r;
}
int test_main(void)
{
    U data[] = {5, 0, 0};
    U memory_expected = 2UL + (2*0x8381UL - (1UL<<16))
        + (2*0x87658381UL - (1UL<<32)) + (0xfedcba9887658381UL ^ (1UL<<32));
    for (U i=0; i < sizeof(pairs)/sizeof(*pairs); ++i)
        if (pairs[i].fn(pairs[i].a, pairs[i].b) != pairs[i].want) return 1;
    for (U i=0; i < sizeof(widths)/sizeof(*widths); ++i)
        if (widths[i].fn(widths[i].a) != widths[i].want) return 2;
    for (U i=0; i<10; ++i)
        if (mir_swap_loop(0xdeadbeef,0x12345678,i) != (i&1 ? 0x12345678 : 0xdeadbeef))
            return 3;
    if (archon_initialized != 0xabcdef9876543210UL || archon_zero != 0) return 4;
    if (mir_memory(data, mir_add, 8) != memory_expected || archon_zero != memory_expected
        || external_word != 5 || data[1] != 1 || data[2] != 1) return 5;
    U ret = mir_outgoing(1,2,3,4,5,6,7,8,9,10,11,data);
    if (last == 0 || ret != last + STACK_REMAINDER || data[0] != 12 || calls != 2)
        return 6;
    return 0;
}
""",
        )))
        self.run_ok(*self.executor(), out)

    def test_checked_traps_and_trap_terminator(self):
        checked = [
            ("add_checked_s", MIN - 1, 1), ("sub_checked_s", MIN, 1),
            ("mul_checked_s", MIN, MASK), ("div_checked_s", MIN, MASK),
            ("div_checked_s", 1, 0),
        ]
        for i, (op, a, b) in enumerate((*checked, ("trap", 0, 0))):
            with self.subTest(op=op, a=a, b=b):
                directory = self.root / f"trap-{i}"
                directory.mkdir()
                if op == "trap":
                    text = "function mir_trap void 0 0 1 0\ntypes\nblock 0 0\nparams\ntrap"
                    harness = "void mir_trap(void); int test_main(void) { mir_trap(); return 0; }"
                else:
                    text = binary(op)
                    harness = (f"unsigned long mir_{op}(unsigned long, unsigned long);\n"
                               f"int test_main(void) {{ return (int)mir_{op}({a:#x}UL, {b:#x}UL); }}")
                out = self.link(directory, (module(text),), harness)
                # No core files: both native Linux and QEMU user mode signal
                # the guest breakpoint with SIGTRAP.
                result = subprocess.run(["sh", "-c", 'ulimit -c 0; exec "$@"',
                                         "test", *self.executor(), out],
                                        capture_output=True, timeout=15, cwd=directory)
                self.assertEqual(result.returncode, -signal.SIGTRAP, result.stderr)


class AMD64NativeGASTests(_ScalarGasTests, unittest.TestCase):
    arch = "amd64"

    def test_pic_addresses_and_scalar_expectations_for_variadic_callees(self):
        compiler = self.compiler()
        directory = self.root / "pic"
        directory.mkdir()
        prog = module(
            """\
function mir_pic i64 8 17 1 0
types i64 i64 i64 i64 i64 i64 i64 i64 ptr i64 func_ptr i64 i64 i64 i64 i64 i64
block 0 9
params
global_addr 8 generic_data
load 9 8 0
func_addr 10 generic_call
call 11 generic_call 8 0 1 2 3 4 5 6 7
call_indirect 12 10 generic_call 8 0 1 2 3 4 5 6 7
call 13 read_al 0
add 14 9 11
add 15 14 12
add 16 15 13
ret 16""",
            """\
function generic_call i64 8 9 1 0
types i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 1
params
const 8 0xbeef
ret 8""",
            imports=("import read_al i64 0",),
            globals_=("global generic_data 1 init 5",),
        )
        code = self.run_ok(self.driver, "--input", "-", "--pic", data=prog)
        self.assertIn(b"generic_data@GOTPCREL(%rip)", code)
        self.assertIn(b"generic_call@GOTPCREL(%rip)", code)
        for target in (b"generic_call@PLT", b"*%r11", b"read_al@PLT"):
            self.assertIn(b"\txorl %eax, %eax\n\tcall " + target, code)
        asm = directory / "pic.s"
        asm.write_bytes(code)
        library = directory / "pic.so"
        self.run_ok(*compiler, "-shared", "-Wl,-z,text,-z,defs", asm,
                    self._write_amd64_read_al(directory), "-o", library)
        harness = directory / "driver.c"
        harness.write_text(r"""
#include <dlfcn.h>
#include <stdarg.h>
typedef unsigned long U;
U generic_data = 31337;
static unsigned calls;
U variadic(U first, ...)
{
    va_list ap;
    U result = 0;
    va_start(ap, first);
    if (first != 1) return 1000000;
    for (unsigned i = 2; i <= 8; ++i) {
        U v = va_arg(ap, U);
        if (v != i) return 2000000;
        result += v * (i+1);
    }
    va_end(ap);
    ++calls;
    return result;
}
int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) return 2;
    U (*fn)(U,U,U,U,U,U,U,U) = (U (*)(U,U,U,U,U,U,U,U))dlsym(lib, "mir_pic");
    if (!fn || fn(1,2,3,4,5,6,7,8) != generic_data + 476 || calls != 2) return 3;
    return 0;
}
""", encoding="ascii")
        interposer = directory / "interpose.s"
        interposer.write_text("""\
.text
.globl generic_call
.type generic_call,%function
generic_call:
    testb %al, %al
    jnz 1f
    jmp variadic
1:
    movl $3000000, %eax
    ret
.section .note.GNU-stack,"",%progbits
""", encoding="ascii")
        out = directory / "driver"
        self.run_ok(*compiler, "-Wall", "-Wextra", "-Werror", "-Wl,--export-dynamic",
                    harness, interposer, "-ldl", "-o", out)
        self.run_ok(out, library)

    @staticmethod
    def _write_amd64_read_al(directory):
        source = directory / "read-al.s"
        source.write_text("""\
.text
.globl read_al
.type read_al,%function
read_al:
    movzbq %al, %rax
    ret
.section .note.GNU-stack,"",%progbits
""", encoding="ascii")
        return source


class AArch64NativeGASTests(_ScalarGasTests, unittest.TestCase):
    arch = "aarch64"

    def test_pic_got_addresses_and_plt_call(self):
        prog = module(
            """\
function mir_pic i64 0 7 1 0
types ptr i64 func_ptr i64 i64 i64 i64
block 0 7
params
global_addr 0 external_data
load 1 0 0
func_addr 2 external_call
call 3 external_call 0
call_indirect 4 2 external_call 0
add 5 1 3
add 6 5 4
ret 6""",
            imports=("import external_call i64 0",),
            globals_=("global external_data 0 zero",),
        )
        directory = self.root / "pic"
        directory.mkdir()
        code = self.emit_asm(prog, pic=True)
        for name in (b"external_data", b"external_call"):
            self.assertIn(b':got:"' + name + b'"', code)
            self.assertIn(b':got_lo12:"' + name + b'"', code)
        asm, obj = directory / "pic.s", directory / "pic.o"
        asm.write_bytes(code)
        self.run_ok(*self.compiler(), "-c", asm, "-o", obj)
        library = directory / "pic.so"
        self.run_ok(self.tool("ld.lld"), "-m", "aarch64linux", "-shared", "-z", "text",
                    obj, "-o", library)
        relocations = self.run_ok(self.tool("readelf"), "-rW", library)
        self.assertRegex(relocations, rb"R_AARCH64_GLOB_DAT[^\n]+external_data")
        self.assertRegex(relocations, rb"R_AARCH64_GLOB_DAT[^\n]+external_call")
        self.assertRegex(relocations, rb"R_AARCH64_JUMP_SLOT[^\n]+external_call")
        harness = """
unsigned long external_data = 10;
unsigned long external_call(void) { return 16; }
unsigned long mir_pic(void);
int test_main(void) { return mir_pic() != 42; }
"""
        out = self.link(directory, (prog,), harness, pic=True)
        self.run_ok(*self.executor(), out)


if __name__ == "__main__":
    unittest.main()
