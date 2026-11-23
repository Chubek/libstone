"""Check C11 bit-field layout, volatile accesses and pointer/object interop with Clang."""

import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "records"
RECORD = "struct S { unsigned x : 3; }; "
NEGATIVE = [
    ("nonconstant", "int width; struct S { int x:width; };", r"integer constant expression"),
    ("negative", "struct S { int x:-1; };", r"nonnegative"),
    ("too-wide", "struct S { int x:33; };", r"exceeds the width"),
    ("wide-bool", "struct S { _Bool x:2; };", r"exceeds the width"),
    ("named-zero", "struct S { int x:0; };", r"zero-width.*unnamed"),
    ("float-width", "struct S { unsigned x:3.0; };", r"integer constant expression"),
    ("enum-type", "enum E { E0 }; struct S { enum E x:3; };", r"bit-field type"),
    ("pointer-type", "struct S { int *x:3; };", r"bit-field type"),
    ("long-type", "struct S { unsigned long x:3; };", r"bit-field type"),
    ("alignas", "struct S { _Alignas(4) unsigned x:3; };", r"_Alignas.*not allowed"),
    ("unnamed-only", "struct S { unsigned :3; unsigned :0; };", r"named member"),
    ("address", RECORD + "unsigned *f(struct S *p) { return &p->x; }", r"address of a bit-field"),
    ("static-address", RECORD + "struct S s; unsigned *p = &s.x;", r"address of a bit-field"),
    ("sizeof", RECORD + "unsigned long f(struct S *s) { return sizeof s->x; }",
     r"sizeof cannot be applied to a bit-field"),
    ("sizeof-generic", RECORD + "struct S s; _Static_assert(sizeof(_Generic(0, int: s.x)), \"x\");",
     r"sizeof cannot be applied to a bit-field"),
    ("offsetof", RECORD + "unsigned long x = __builtin_offsetof(struct S, x);",
     r"offsetof cannot name a bit-field"),
    ("const", RECORD + "void f(const struct S *s) { s->x = 1; }",
     r"modifiable|const-qualified"),
    ("const-member-copy", "struct S { const unsigned x:3; }; "
     "void f(struct S *a, struct S *b) { *a = *b; }", r"const-qualified"),
    ("const-volatile-member", "struct S { const volatile unsigned x:3; }; "
     "void f(struct S *s) { s->x = 2; }", r"modifiable|const-qualified"),
    ("by-value-param", RECORD + "int f(struct S s) { return s.x; }",
     r"by-value aggregate ABI.*bit-fields"),
    ("by-value-result", RECORD + "struct S f(struct S *p) { return *p; }",
     r"by-value aggregate ABI.*bit-fields"),
    ("by-value-nested", RECORD + "struct T { struct S v[2]; }; "
     "int f(int (*p)(struct T), struct T *v) { return p(*v); }",
     r"by-value aggregate ABI.*bit-fields"),
]

PASSES = ("--qml-pass=canonicalize", "--qml-pass=strength_reduce")


def memory_accesses(text, function):
    body = re.search(rf"(?ms)^function {function}\b.*?(?=^(?:function|import|global|end)\b)",
                     text).group()
    return re.findall(r"(?m)^\s*((?:load_u|store)(?:8|16|32))\b[^\n]*? (-?\d+)$", body)


def expected_accesses(aarch64):
    def read(*chunks):
        return [(f"load_u{width}", str(offset)) for width, offset in chunks]

    def write(*chunks):
        return [(f"store{width}", str(offset)) for width, offset in chunks]

    word, byte = ((32, 0),), ((8, 0),)
    delta, wide = (word, word) if aarch64 else (((8, 1), (8, 2)), ((16, 0), (8, 2)))
    delta_store = read(*(delta if aarch64 else delta[1:])) + write(*delta)
    delta_update = read(*delta) + (read(*delta) if aarch64 else []) + write(*delta)
    sequence = (read(*delta) + delta_store + delta_update * 2 + read(*byte) + write(*byte)
                + (read(*word) if aarch64 else []) + write(*word) + read(*word)
                + read(*(word if aarch64 else byte)) * 2)
    wide_sequence = (read(*wide) + (read(*wide) if aarch64 else []) + write(*wide)
                     + read(*wide) + (read(*wide) if aarch64 else []) + write(*wide))
    result = {"volatile_access_sequence": sequence, "volatile_wide_sequence": wide_sequence}
    split = ((8, 1), (16, 2))
    result["volatile_split_sequence"] = read(*split) * (3 if aarch64 else 2) + write(*split)
    field = word if aarch64 else byte
    result["volatile_union_sequence"] = (read(*field) * 2 + write(*field) +
                                         read(*field) * (2 if aarch64 else 1) + write(*field))
    result["volatile_separate_sequence"] = read(*byte, (8, 1)) + write((8, 1))
    return result


def native_memory_accesses(text, function, triple):
    body = re.search(rf'(?ms)^"?{function}"?:.*?^\s*\.size\b', text).group()
    if triple.startswith("aarch64"):
        patterns = {
            "load_u8": r"ldrb w9, \[x9\]", "load_u16": r"ldrh w9, \[x9\]", "load_u32": r"ldr w9, \[x9\]",
            "store8": r"strb w10, \[x9\]", "store16": r"strh w10, \[x9\]", "store32": r"str w10, \[x9\]",
        }
    elif triple.startswith("x86"):
        patterns = {
            "load_u8": r"movzbq \d*\(%rax\), %rax", "load_u16": r"movzwq \d*\(%rax\), %rax",
            "load_u32": r"movl \d*\(%rax\), %eax", "store8": r"movb %cl, \d*\(%rax\)",
            "store16": r"movw %cx, \d*\(%rax\)", "store32": r"movl %ecx, \d*\(%rax\)",
        }
    else:
        patterns = {"load_u8": r"lbu .*", "load_u16": r"lhu .*", "load_u32": r"lwu .*",
                    "store8": r"sb .*", "store16": r"sh .*", "store32": r"sw .*"}
    return [op for line in body.splitlines() for op, pattern in patterns.items()
            if re.fullmatch(pattern, line.strip())]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--qemu")
    parser.add_argument("--qemu-aarch64")
    args = parser.parse_args()
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None, ()),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64, ()),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu, ("-march=rv64im", "-mabi=lp64", "-mno-relax")),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-bitfields-worker3-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))

        def check(*command, expected=None):
            p = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, timeout=300)
            if expected is None:
                if p.returncode != 0:
                    raise AssertionError((command, p.returncode, p.stdout, p.stderr))
            elif p.returncode <= 0 or not re.search(expected, p.stderr):
                raise AssertionError((command, expected, p.returncode, p.stdout, p.stderr))

        def run(exe, hosts, emulator):
            if host in hosts:
                check(exe)
            elif emulator:
                check(emulator, exe)
            else:
                print(f"{exe.name}: linked, execution skipped (no emulator)")

        for triple, hosts, emulator, abi in targets:
            cephyr = (args.cephyr.resolve(), "-std=c11", f"--target={triple}")
            fixture, foreign = FIXTURES / "bitfields.c", FIXTURES / "bitfields_external.c"
            volatile = FIXTURES / "volatile_bitfields.c"
            cephyr_obj, external_obj, exe, volatile_exe, mir, asm = (
                work / f"{triple}-{s}" for s in ("native.o", "ext.o", "main", "volatile", "volatile.mir", "volatile.s")
            )
            external_cc = ((args.clang, "-std=c11", f"--target={triple}", *abi, "-O2",
                            "-ffreestanding", "-fno-pie", "-fno-stack-protector") if args.clang else cephyr)
            check(*external_cc, "-c", foreign, "-o", external_obj)
            check(*cephyr, "-c", fixture, "-o", cephyr_obj)
            check(*cephyr, "--stdlib=cephyr", cephyr_obj, external_obj, "-o", exe)
            run(exe, hosts, emulator)
            expected = expected_accesses(triple.startswith("aarch64"))
            for passes in ((), PASSES):
                check(*cephyr, *passes, "--emit-mir", volatile, "-o", mir)
                check(*cephyr, *passes, "-S", volatile, "-o", asm)
                mir_text, asm_text = (output.read_text(encoding="ascii") for output in (mir, asm))
                for function, accesses in expected.items():
                    actual = memory_accesses(mir_text, function)
                    if actual != accesses:
                        raise AssertionError((triple, passes, function, accesses, actual))
                    native = native_memory_accesses(asm_text, function, triple)
                    if native != [op for op, _ in accesses]:
                        raise AssertionError((triple, passes, function, accesses, native))
                check(*cephyr, *passes, "--stdlib=cephyr", volatile, "-o", volatile_exe)
                run(volatile_exe, hosts, emulator)
            for name, text, diagnostic in NEGATIVE:
                source, output = work / f"{name}.c", work / f"{triple}-{name}.mir"
                source.write_text(text + "\n", encoding="ascii")
                check(*cephyr, "--emit-mir", source, "-o", output, expected=diagnostic)
                if output.exists():
                    raise AssertionError(f"unexpected output: {output}")


if __name__ == "__main__":
    main()
