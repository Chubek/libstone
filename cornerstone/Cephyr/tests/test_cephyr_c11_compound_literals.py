"""Check C11 compound literal initialization, type, lvalue and storage semantics."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


FIXTURE = Path(__file__).resolve().parent / "fixtures" / "records" / "compound_literals.c"
NEGATIVE = [
    ("void", "void f(void) { (void){1}; }", r"complete object type"),
    ("function", "typedef int F(void); void f(void) { (F){1}; }", r"complete object type"),
    ("incomplete", "struct S; void f(void) { (struct S){1}; }", r"complete object type"),
    ("vla", "void f(int n) { (int[n]){1}; }", r"variable-length array"),
    ("empty", "void f(void) { (int){}; }", r"expected expression"),
    ("excess", "void f(void) { (int){1, 2}; }", r"exactly one expression"),
    ("array-bound", "void f(void) { (int[2]){[2]=1}; }", r"outside the array bound"),
    ("scalar-designation", "void f(void) { (int){[0]=1}; }", r"scalar initializer.*designator"),
    ("const", "void f(void) { (const int){1} = 2; }", r"const-qualified|modifiable"),
    ("unevaluated", "int f(void) { return sizeof (int []){(int *)0}; }",
     r"incompatible compound literal initializer"),
    ("block-static", "int f(void) { static int *p = (int []){1}; return *p; }",
     r"pointer initializer"),
    ("file-runtime", "int f(void); int *p = (int []){f()};", r"arithmetic constant expression"),
    ("file-unevaluated", "int f(void); unsigned long n = sizeof (int){f()};",
     r"arithmetic constant expression"),
    ("not-an-ice", "_Static_assert((int){1}, \"not an ICE\");", r"integer constant expression"),
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
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-compound-literals-worker4-") as directory:
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
