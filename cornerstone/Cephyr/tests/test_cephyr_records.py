"""Exercise native C11 structure/union layout, copies and constraints."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


FIXTURE = Path(__file__).resolve().parent / "fixtures" / "records" / "native.c"
FLEXIBLE = FIXTURE.with_name("flexible.c")

NEGATIVE = [
    ("unknown", "struct R { int n; }; int f(struct R *p) { return p->missing; }",
     r"no such member"),
    ("incomplete", "struct R; int f(struct R *p) { return p->n; }", r"complete structure"),
    ("incomplete-element", "struct R; extern struct R a[2];", r"incomplete structure|complete object"),
    ("incomplete-static", "static struct R r; struct R { int n; };", r"internal tentative"),
    ("self", "struct R { struct R member; };", r"complete object type"),
    ("flexible-only", "struct R { char bytes[]; };", r"preceding named member"),
    ("flexible-nonfinal", "struct R { int n; char bytes[], last; };", r"last member"),
    ("flexible-union", "union U { int n; char bytes[]; };", r"requires a structure"),
    ("flexible-array-element", "struct R { int n; char bytes[]; }; struct R a[2];",
     r"array element type cannot contain a flexible array"),
    ("flexible-array-typedef", "struct R { int n; char bytes[]; }; typedef struct R A[];",
     r"array element type cannot contain a flexible array"),
    ("flexible-contained", "struct R { int n; char bytes[]; }; struct S { struct R r; };",
     r"structure member cannot contain a flexible array"),
    ("flexible-recursive-union-array", "struct R { int n; char bytes[]; };"
     "union U { struct R r; }; union V { union U u; }; union V a[2];",
     r"array element type cannot contain a flexible array"),
    ("flexible-recursive-union-member", "struct R { int n; char bytes[]; };"
     "union U { struct R r; }; union V { union U u; }; struct S { union V v; };",
     r"structure member cannot contain a flexible array"),
    ("flexible-incomplete-element", "struct T; struct R { int n; struct T bits[]; };",
     r"incomplete structure|complete element"),
    ("flexible-initialized", "struct R { int n; char bytes[]; }; struct R r = {1, \"a\"};",
     r"too many elements|flexible array member cannot be initialized"),
    ("flexible-designated", "struct R { int n; char bytes[]; };"
     "void f(void) { struct R r = {.n = 1, .bytes[0] = 'x'}; }",
     r"flexible array member cannot be initialized"),
    ("flexible-sizeof", "struct R { int n; char bytes[]; };"
     "unsigned long f(struct R *p) { return sizeof p->bytes; }",
     r"sizeof requires a complete object type"),
    ("flexible-qualified", "struct R { int n; char bytes[]; };"
     "void f(const struct R *p) { p->bytes[0] = 'x'; }",
     r"modifiable|const-qualified"),
    ("const", "struct R { int n; }; void f(const struct R *p) { p->n = 3; }",
     r"modifiable|const-qualified"),
    ("nested-const", "struct Q { const int n; }; struct R { struct Q q[2]; };"
     "void f(struct R *p) { *p = *p; }", r"const-qualified"),
    ("volatile-union-members", "union U { volatile int n; long w; }; "
     "void f(union U *p, union U *q) { *q = *p; }",
     r"whole-object copy of a union with volatile members"),
    ("too-many", "union U { int a; int b; }; union U x = {1, 2};", r"too many elements"),
    ("global-copy", "struct R { int n; }; struct R a = {1}, b = a;", r"must use braces"),
    ("register", "struct R { int n; }; int f(void) { register struct R r; return &r.n != 0; }",
     r"address of a register"),
    ("non-lvalue", "struct R { int n; }; int *f(struct R *p) { return &(*p = *p).n; }",
     r"not an lvalue"),
    ("distinct-tags", "struct R { int n; }; void f(struct R *p) {"
     "struct R { int n; } r; r = *p; }", r"compatible complete"),
    ("by-value-overaligned", "struct R { _Alignas(16) long n; }; void f(struct R r) {}",
     r"by-value aggregate"),
    ("bit-field", "struct R { unsigned n : 3; };", r"bit.field|expected"),
    ("negative-index", "int a[] = {[-1] = 3};", r"nonnegative"),
    ("nonconstant-index", "int f(int n) { int a[2] = {[n] = 3}; return a[0]; }",
     r"integer constant expression"),
    ("outside-array", "int a[2] = {[2] = 3};", r"outside the array bound"),
    ("overflow-index", "int a[] = {[0xfffffffffffffffful] = 3};", r"object size limit"),
    ("member-in-array", "int a[1] = {.member = 3};", r"member designator"),
    ("array-in-scalar", "struct R { int n; }; struct R r = {.n[1] = 3};",
     r"array designator requires an array"),
]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu-aarch64")
    parser.add_argument("--qemu")
    args = parser.parse_args()
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-records-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env["LC_ALL"] = "C"
        env["XDG_CACHE_HOME"] = str(work / "cache")
        for triple, hosts, emulator in targets:
            command = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]
            for fixture in (FIXTURE, FLEXIBLE):
                executable = work / f"{triple}-{fixture.stem}"
                subprocess.run([*command, fixture, "-o", executable], cwd=work, env=env, check=True)
                if host in hosts:
                    subprocess.run([executable], cwd=work, env=env, check=True)
                elif emulator:
                    subprocess.run([emulator, executable], cwd=work, env=env, check=True)
                else:
                    print(f"{triple}: {fixture.stem} linked, execution skipped (no emulator)")
            for name, source, diagnostic in NEGATIVE:
                bad = work / f"{name}.c"
                bad.write_text(source + "\n", encoding="ascii")
                output = work / f"{name}-{triple}.s"
                result = subprocess.run([*command, "-S", bad, "-o", output],
                                        cwd=work, env=env, capture_output=True, text=True)
                if result.returncode <= 0 or output.exists() or not re.search(diagnostic, result.stderr):
                    raise AssertionError(f"{triple} {name}: expected {diagnostic!r}; "
                                         f"exit {result.returncode}\n{result.stderr}")


if __name__ == "__main__":
    main()
