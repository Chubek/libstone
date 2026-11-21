"""Test C11 array parameter adjustment and bound constraints on three targets."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


PROGRAM = Path(__file__).resolve().parent / "fixtures" / "c11_array_parameters.c"

REJECTS = (
    # §6.7.6.2p1: the bracket qualifiers/static are outer parameter syntax.
    ("int a[const 3];", "outermost array derivation"),
    ("int a[static 3];", "outermost array derivation"),
    ("void f(int a[2][const 3]);", "outermost array derivation"),
    ("void f(int (*a)[static 2]);", "outermost array derivation"),
    ("int f(void) { return sizeof(int [restrict 1]); }", "outermost array derivation"),
    ("void f(int a[static]);", "static array bound requires a size expression"),
    ("void f(int a[static *]);", "static array bound requires a size expression"),
    ("void f(int a[_Atomic 1]);", "_Atomic types are unsupported"),
    ("void f(int a[static 0]);", "array bound must be positive"),
    ("void f(int a[-1]);", "array bound must be positive"),
    ("void f(int a[1.5]);", "array bound must be a valid integer expression"),
    ("void f(int *p, int a[p]);", "array bound must be a valid integer expression"),
    ("void f(int a[n], int n);", "no valid declaration for 'n'"),
    ("void f(void a[1]);", "array element type must be a complete object type"),
    ("struct Incomplete; void f(struct Incomplete a[1]);", "incomplete structure or union"),
    ("void f(int a[1][]);", "array element type must be a complete object type"),
    # §6.7.6.2p4/p5 and §6.7.6.3p12: stars require prototype scope;
    # nonconstant bounds of definitions would require entry evaluation.
    ("int a[*];", "at function prototype scope"),
    ("typedef int Array[*];", "at function prototype scope"),
    ("void f(int a[*]) {}", "at function prototype scope"),
    ("void f(int n, int a[n++]) {}", "nonconstant array parameter bounds in function definitions"),
    ("int effect(void); void f(int a[static effect()]) {}", "nonconstant array parameter bounds in function definitions"),
    # These would retain a VLA behind the pointer even after adjustment.
    ("void f(int a[1][*]);", "only supported on an outermost array parameter"),
    ("void f(int (*a)[*]);", "only supported on an outermost array parameter"),
    ("void f(int n, int a[1][n]);", "variable-length arrays are not supported"),
    ("void f(int n, int (*a)[n]) {}", "variable-length arrays are not supported"),
    # §6.7.6.3p4/p7/p15: distinguish the pointer object and pointee.
    ("void f(int [const 1]) {}", "requires named parameters"),
    ("struct Incomplete; void f(struct Incomplete a) {}", "complete type after adjustment"),
    ("void f(int a[const 1]) { a = 0; }", "modifiable scalar lvalue"),
    ("void f(const int a[1]) { a[0] = 1; }", "modifiable scalar lvalue"),
    ("void f(int a[restrict 1]) { int **p = &a; }", "incompatible object-pointer conversion"),
    ("void f(register int a[1]) { (void)&a; }", "cannot take the address of a register object"),
    ("void f(int a[const 1]); void f(const int *a);", "conflicting types for 'f'"),
    ("void f(int a[1][2]); void f(int (*a)[3]);", "conflicting types for 'f'"),
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu")
    parser.add_argument("--qemu-aarch64")
    args = parser.parse_args()
    host = platform.machine().lower()
    targets = (
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    )
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-array-parameters-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        bad = work / "reject.c"
        for triple, hosts, emulator in targets:
            command = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]
            executable = work / triple
            subprocess.run([*command, PROGRAM, "-o", executable],
                           cwd=work, env=env, check=True)
            if host in hosts or emulator:
                run = [executable] if host in hosts else [emulator, executable]
                subprocess.run(run, cwd=work, env=env, check=True)
            else:
                print(f"{triple}: linked, execution skipped (no emulator)")

            for source, diagnostic in REJECTS:
                bad.write_text(source + "\n", encoding="ascii")
                result = subprocess.run([*command, "-fsyntax-only", bad], cwd=work,
                                        env=env, capture_output=True, text=True)
                if result.returncode <= 0 or diagnostic not in result.stderr:
                    raise AssertionError(f"{triple}: {source}\nExpected {diagnostic!r}\n{result.stderr}")


if __name__ == "__main__":
    main()
