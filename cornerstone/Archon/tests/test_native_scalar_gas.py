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


def fp_function(name, result, params, values, *insts):
    return "\n".join((
        f"function {name} {result} {len(params)} {len(params) + len(values)} 1 0",
        "types " + " ".join((*params, *values)), f"block 0 {len(insts) - 1}", "params",
        *insts,
    ))


def floating_program():
    functions, imports, harness = [], [], []
    for fmt, ctype, cname in (("f32", "float", "f32"), ("f64", "double", "f64")):
        for op, expr in (
            ("fneg", "-a"), ("fadd", "a+b"), ("fsub", "a-b"), ("fmul", "a*b"),
            ("fdiv", "a/b"), ("feq", "a==b"), ("flt", "a<b"), ("fle", "a<=b"),
            ("fcast", "(double)a" if fmt == "f32" else "(float)a"),
            ("sitofp", f"({ctype})a"), ("uitofp", f"({ctype})a"),
            ("fptosi", "(i64)a"), ("fptoui", "(u64)a"),
        ):
            name = f"mir_{op}_{fmt}"
            binary_op = op in {"fadd", "fsub", "fmul", "fdiv", "feq", "flt", "fle"}
            inputs = (
                ("i64",) if op in ("sitofp", "uitofp") else
                (fmt, fmt) if binary_op else (fmt,)
            )
            output = (
                "i64" if op in ("feq", "flt", "fle", "fptosi", "fptoui") else
                ("f64" if fmt == "f32" else "f32") if op == "fcast" else fmt
            )
            functions.append(fp_function(name, output, inputs, (output,),
                f"{op} {len(inputs)} 0" + (" 1" if binary_op else ""), f"ret {len(inputs)}"))
            c_inputs = (["i64"] if op == "sitofp" else ["u64"] if op == "uitofp" else
                        [ctype, ctype] if binary_op else [ctype])
            c_output = ("u64" if op == "fptoui" else
                        {"i64": "i64", "f32": "float", "f64": "double"}[output])
            harness.append(f"extern {c_output} {name}({', '.join(c_inputs)});")
            if op in ("sitofp", "uitofp"):
                sample = "si" if op == "sitofp" else "ui"
            elif op in ("fptosi", "fptoui"):
                sample = f"{cname}_" + ("signed" if op == "fptosi" else "unsigned")
            else:
                sample = cname
            expected_args = "a, b" if binary_op else "a"
            check = (f"same_{output}({name}({expected_args}), {expr})" if output != "i64"
                     else f"({name}({expected_args}) == ({expr}))")
            harness.append(f"""
static int check_{op}_{fmt}(void)
{{
    for (unsigned i = 0; i < sizeof {sample} / sizeof *{sample}; ++i) {{
        volatile {c_inputs[0]} a = {sample}[i];
        {"for (unsigned j = 0; j < sizeof " + sample + " / sizeof *" + sample + "; ++j) {" if binary_op else ""}
        {"volatile " + ctype + " b = " + sample + "[j];" if binary_op else ""}
        if (!{check}) return 1;
        {"}" if binary_op else ""}
    }}
    return 0;
}}""")
    functions.append("""\
function mir_fp_memory f32 3 8 2 0
types ptr f32 f64 f32 f64 f32 i64 f32
block 0 6
params
fstore 0 1 0xfffffffffffffff8
fstore 0 2 0
fload 3 0 0xfffffffffffffff8
fload 4 0 0
fcast 5 4
feq 6 5 5
cbr 6 1 1 3 1 1 5
block 1 0
params 7
ret 7""")
    functions.append(fp_function("mir_fp_constants", "f64", (), ("f64", "f32", "f64", "f64"),
        "fconst 0 0x3ff8000000000000", "fconst 1 0x3e800000", "fcast 2 1", "fmul 3 0 2", "ret 3"))
    functions.append(fp_function("mir_nan_sign", "f32", (), ("f32", "f32"),
        "fconst 0 0x7fc0abcd", "fneg 1 0", "ret 1"))
    # Exercise both banks independently, with their overflow slots interleaved.
    # Reordering also prevents a wrong-but-symmetric incoming/outgoing layout
    # from cancelling itself. The large local tests far incoming offsets.
    count = 12
    mixed = [t for i in range(count) for t in ("i64", "f64" if i % 2 else "f32")]
    c_types = [{"i64": "u64", "f32": "float", "f64": "double"}[t] for t in mixed]
    for indirect in (False, True):
        offset = int(indirect)
        op = "call_indirect" if indirect else "call"
        result = "f32" if indirect else "f64"
        c_result = "float" if indirect else "double"
        target = "host_mixed_f32" if indirect else "host_mixed_f64"
        params = (["func_ptr"] if indirect else []) + mixed
        args = " ".join(str(2 * ((i + 4) % count) + j + offset)
                        for i in range(count) for j in range(2))
        caller = f"mir_mixed_{result}"
        n = len(params)
        functions.append(f"""\
function {caller} {result} {n} {n + 1} 1 1
types {' '.join(params)} {result}
local 40000 16
block 0 1
params
{op} {n} {'0 ' if indirect else ''}{target} {len(mixed)} {args}
ret {n}""")
        imports.append(f"import {target} {result} {len(mixed)} {' '.join(mixed)}")
        typed = ", ".join(f"{t} p{i}" for i, t in enumerate(c_types))
        expect = " && ".join(
            f"p{2*i} == {100 + ((i+4) % count)} && p{2*i+1} == {((i+4) % count) + 0.25}"
            for i in range(count)
        )
        harness.append(f"{c_result} {target}({typed}) {{ return ({expect}) ? 6.25 : 0; }}")
        host_sig = ", ".join(c_types)
        harness.append(f"extern {c_result} {caller}("
                       + (f"{c_result} (*)({host_sig}), " if indirect else "") + host_sig + ");")
        arg_text = ", ".join(f"{100 + i}, {i + 0.25}" for i in range(count))
        harness.append(f"static int check_{caller}(void) {{ return "
                       f"{caller}({target + ', ' if indirect else ''}{arg_text}) != 6.25; }}")
    return module(*functions, imports=imports), "\n".join(harness)


FP_HARNESS_PREAMBLE = """\
typedef signed long i64;
typedef unsigned long u64;
typedef unsigned u32;
static u32 bits32(float x) { u32 r; __builtin_memcpy(&r, &x, 4); return r; }
static u64 bits64(double x) { u64 r; __builtin_memcpy(&r, &x, 8); return r; }
static int same_f32(float x, float y) {
    return bits32(x) == bits32(y) || (x != x && y != y);
}
static int same_f64(double x, double y) {
    return bits64(x) == bits64(y) || (x != x && y != y);
}
static const float f32[] = {
    0, -0.0f, 0x1p-149f, -0x1p-126f, -1.125f, 1.5f, 0x1.fffffep127f,
    __builtin_inff(), -__builtin_inff(), __builtin_nanf("0x26")
};
static const double f64[] = {
    0, -0.0, 0x1p-1074, -0x1p-1022, -1.125, 1.5, 0x1.ffffffp0, 0x1.fffffffffffffp1023,
    __builtin_inf(), -__builtin_inf(), __builtin_nan("0x26")
};
static const i64 si[] = {0, -1, 0x1000001, -0x1000001, 0x20000000000001,
                        0x7fffffffffffffffL, -0x7fffffffffffffffL-1};
static const u64 ui[] = {0, 1, 0x1000001, 0x20000000000001, 0x7fffffffffffffffUL,
                       0x8000000000000000UL, 0x8000008000000001UL, 0x8000000000000401UL,
                       0x8000000000000400UL, 0xfffffffffffffffeUL, 0xffffffffffffffffUL};
static const float f32_signed[] = {-0x1p63f, -43.75f, -0.5f, 0, 87.125f, 0x1.fffffep62f};
static const double f64_signed[] = {-0x1p63, -43.75, -0.5, 0, 87.125, 0x1.fffffffffffffp62};
static const float f32_unsigned[] = {-0.75f, 0, 87.125f, 0x1p63f, 0x1.fffffep63f};
static const double f64_unsigned[] = {-0.75, 0, 87.125, 0x1p63, 0x1.fffffffffffffp63};
extern float mir_fp_memory(double *, float, double);
extern double mir_fp_constants(void);
extern float mir_nan_sign(void);
static int check_objects(void) {
    struct { float f; u32 guard; double d; u64 after; } data = {1, 42, 6, 17};
    float from_mem = mir_fp_memory(&data.d, -0.0f, 3.5);
    return data.guard != 42 || data.after != 17 || data.d != 3.5 ||
           bits32(data.f) != 0x80000000U || bits32(from_mem) != 0x80000000U ||
           mir_fp_constants() != 0.375 || bits32(mir_nan_sign()) != 0xffc0abcdU;
}
"""


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
        work = tempfile.TemporaryDirectory(prefix=f"archon-{cls.arch}-")
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
        self.assertEqual(manifest["floating_point"]["formats"], ["IEEE binary32", "IEEE binary64"])
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

    def test_floating_point_hardware_and_abi(self):
        program, checks = floating_program()
        names = [f"check_{op}_{fmt}" for fmt in ("f32", "f64") for op in
            "fneg fadd fsub fmul fdiv feq flt fle fcast sitofp uitofp fptosi fptoui".split()]
        names += ["check_objects", "check_mir_mixed_f64", "check_mir_mixed_f32"]
        main = "\nint test_main(void) {\n" + "".join(
            f"    if ({name}()) return {i + 1};\n" for i, name in enumerate(names)
        ) + "    return 0;\n}\n"
        out = self.link(self.root, [program], FP_HARNESS_PREAMBLE + checks + main, pic=True)
        result = self.invoke(*self.executor(), out)
        self.assertEqual(result.returncode, 0,
            (names[result.returncode - 1] if 1 <= result.returncode <= len(names) else result.returncode,
             result.stdout, result.stderr))

    def test_floating_point_verifier_rejects_mistyped_operations(self):
        malformed = (
            (fp_function("bad", "f32", (), ("f32",), "fconst 0 0x100000000", "ret 0"),
             b"f32 bits exceed 32 bits"),
            (fp_function("bad", "f32", ("f32", "f64"), ("f32",), "fadd 2 0 1", "ret 2"),
             b"matching FP types"),
            (fp_function("bad", "f32", ("ptr",), ("f32",), "fload 1 0 2", "ret 1"),
             b"does not satisfy access alignment"),
            (fp_function("bad", "f64", ("f64",), ("f64",), "fcast 1 0", "ret 1"),
             b"different FP source and result types"),
            (fp_function("bad", "i64", ("f64", "f64"), ("i64",), "eq 2 0 1", "ret 2"),
             b"integer operation requires"),
        )
        for func, message in malformed:
            with self.subTest(message=message):
                res = self.invoke(self.driver, "--input", "-", data=module(func))
                self.assertEqual((res.returncode, res.stdout), (1, b""))
                self.assertIn(message, res.stderr)
        func = fp_function("bad", "f64", ("f32",), ("f64",), "call 1 host 1 0", "ret 1")
        res = self.invoke(self.driver, "--input", "-", data=module(func,
            imports=("import host f64 1 f64",)))
        self.assertEqual((res.returncode, res.stdout), (1, b""))
        self.assertIn(b"call argument type does not match callee", res.stderr)

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
    static const ArchonMIRType fp_types[] = { ARCHON_MIR_F32, ARCHON_MIR_F32 };
    static const ArchonMIRInst fp_code[] = {
        {ARCHON_MIR_FNEG, 1, 0, ARCHON_MIR_NONE, 0, 0, 0, 0}
    };
    static const ArchonMIRBlock fp_block[] = {
        {0, 0, fp_code, 1, {ARCHON_MIR_RETURN, 1, {0, 0, 0}, {0, 0, 0}}}
    };
    static const ArchonMIRFunction fp_function[] = {
        {"negate", ARCHON_MIR_F32, fp_types, 2, 1, fp_block, 1, 0, 0}
    };
    static const ArchonMIRModule fp_module = { ARCHON_MIR_SCHEMA_VERSION, fp_function, 1, 0, 0, 0, 0 };
    static const char fp_text[] =
        "archon-mir 1 module 1 0 0 function negate f32 1 2 1 0 "
        "types f32 f32 block 0 1 params fneg 1 0 ret 1 end";
    static const ArchonMIRModule m = { ARCHON_MIR_SCHEMA_VERSION, fs, 1, 0, 0, 0, 0 };
    ArchonBackendArtifact a = {0}, t = {0};
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
    assert(archon_backend_compile(&fp_module, 0, &a, &d) == ARCHON_BACKEND_OK);
    assert(archon_backend_compile_text(fp_text, sizeof fp_text - 1u, 0, &t, &d) == ARCHON_BACKEND_OK);
    assert(a.size == t.size && memcmp(a.data, t.data, a.size) == 0);
    archon_backend_artifact_destroy(&a);
    archon_backend_artifact_destroy(&t);
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

    def test_sysv_fp_call_to_compatible_variadic_import(self):
        # MIR still has a fixed scalar signature. These arguments already
        # obey C's default promotions: all FP actuals are double.
        types = ("i64",) + ("f64",) * 10
        args = " ".join(map(str, range(len(types))))
        prog = module(
            fp_function("mir_variadic_fp", "f64", types, ("f64",),
                        f"call 11 variadic_fp 11 {args}", "ret 11"),
            imports=(f"import variadic_fp f64 11 {' '.join(types)}",),
        )
        self.assertIn(b"\tmovl $8, %eax\n\tcall variadic_fp", self.emit_asm(prog))
        directory = self.root / "variadic-fp"
        directory.mkdir()
        out = self.link(directory, [prog], """\
#include <stdarg.h>
extern double mir_variadic_fp(unsigned long, double, double, double, double,
                              double, double, double, double, double, double);
double variadic_fp(unsigned long n, ...)
{
    double sum = 0;
    va_list ap;
    va_start(ap, n);
    for (unsigned long i = 1; i <= n; ++i) {
        double x = va_arg(ap, double);
        sum += x * i;
    }
    va_end(ap);
    return sum;
}
int test_main(void)
{
    return mir_variadic_fp(10, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5, 10.5) != 412.5;
}
""")
        self.run_ok(out)

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
