"""Check C11 anonymous member storage, promotion, initialization and guards."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


FIXTURE = Path(__file__).resolve().parent / "fixtures" / "records" / "anonymous.c"
NEGATIVE = [
    ("tagged", "struct A { struct T { int n; }; };", r"tagless struct or union"),
    ("typedef", "typedef union { int n; } U; struct A { U; };", r"tagless struct or union"),
    ("missing", "struct A { int; };", r"tagless struct or union"),
    ("named-then-promoted", "struct A { int n; union { int n; }; };", r"duplicate record member"),
    ("promoted-then-named", "struct A { union { int n; }; int n; };", r"duplicate record member"),
    ("promoted-twice", "struct A { union { int n; }; struct { int n; }; };",
     r"duplicate record member"),
    ("not-promoted", "struct A { struct { struct { int n; } named; }; };"
     "int f(struct A *p) { return p->n; }", r"no such member"),
    ("unknown-designator", "struct A { struct { int n; }; };"
     "struct A a = { .m = 4 };", r"member designator"),
    ("nested-flexible", "struct A { union { struct { int n; char data[]; }; }; };",
     r"structure member cannot contain a flexible array"),
    ("const", "struct A { const struct { union { int n; }; }; };"
     "void f(struct A *p) { p->n = 1; }", r"modifiable|const-qualified"),
    ("const-copy", "struct A { const struct { int n; }; };"
     "void f(struct A *p) { *p = *p; }", r"modifiable|const-qualified"),
    ("register", "struct A { struct { int n; }; };"
     "int f(void) { register struct A a; return &a.n != 0; }", r"address of a register"),
    ("abi-parameter", "struct A { union { int n; }; }; int f(struct A a) { return a.n; }",
     r"by-value aggregate ABI.*anonymous"),
    ("abi-return", "struct A { struct { int n; }; };"
     "struct A f(void) { struct A a = {0}; return a; }", r"by-value aggregate ABI.*anonymous"),
    ("abi-array", "struct A { struct { int n; }; }; struct B { struct A a[1]; };"
     "int f(struct B b) { return b.a[0].n; }", r"by-value aggregate ABI.*anonymous"),
    ("abi-direct", "struct A { struct { int n; }; }; int take(struct A);"
     "int f(struct A *arg) { return take(*arg); }", r"by-value aggregate ABI.*anonymous"),
    ("abi-indirect", "struct A { struct { int n; }; };"
     "int f(int (*p)(struct A), struct A *arg) { return p(*arg); }",
     r"by-value aggregate ABI.*anonymous"),
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
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-anonymous-worker6-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        for triple, hosts, emulator in targets:
            command = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]
            executable = work / triple
            subprocess.run([*command, FIXTURE, "-o", executable], cwd=work, env=env, check=True)
            if host in hosts or emulator:
                launch = [executable] if host in hosts else [emulator, executable]
                subprocess.run(launch, cwd=work, env=env, check=True)
            else:
                print(f"{triple}: linked, execution skipped (no emulator)")
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
