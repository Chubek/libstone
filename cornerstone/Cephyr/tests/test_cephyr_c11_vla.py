"""Exercise C11 VLAs through the compiler, native backends, and executable ABI."""

import argparse
import os
from pathlib import Path
import platform
import resource
import signal
import subprocess
import tempfile


FIXTURE = Path(__file__).resolve().parent / "fixtures" / "c11_vla.c"
REJECTS = (
    ("int n; int a[n];", "variable-length arrays are not supported"),
    ("void f(int n) { int a[n] = {1}; }", "cannot have an initializer"),
    ("void f(int n) { static int a[n]; }", "require automatic storage"),
    ("void f(int n) { extern int a[n]; }", "require automatic storage"),
    ("void f(float n) { int a[n]; }", "valid integer expression"),
    ("void f(int n) { int a[][n]; }", "incomplete local array"),
    ("void f(int n) { typedef int A[n]; A a; }", "variable-length arrays are not supported"),
    ("void f(int n) { int (*p)[n]; }", "variable-length arrays are not supported"),
    ("void f(int n) { struct S { int a[n]; }; }", "variable-length arrays are not supported"),
    ("void f(int n) { goto in; { int a[n]; in: ; } }", "goto enters the scope"),
    ("void f(int n) { switch (n) { int a[n]; case 1: break; } }", "switch label enters the scope"),
    ("void f(int n) { int a[n]; _Static_assert(sizeof a > 0, \"bad\"); }",
     "integer constant expression"),
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
    # Invalid bounds intentionally trap; never leave a core artifact behind.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-vla-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        source = work / "case.c"
        executable = work / "program"
        for triple, hosts, emulator in targets:
            command = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]

            def invoke(*argv, **kwargs):
                return subprocess.run([*command, *argv], cwd=work, env=env, **kwargs)

            runner = [] if host in hosts else [emulator] if emulator else None
            mir = invoke("--emit-mir", FIXTURE, "-o", "-", capture_output=True, check=True).stdout
            assert b"stack_alloc " in mir and b"stack_free " in mir, triple
            for mode, options in (("plain", ()), ("qml", ("--qml-pass=canonicalize",
                                                          "--qml-pass=strength_reduce"))):
                invoke(*options, FIXTURE, "-o", executable, check=True)
                if runner is not None:
                    subprocess.run([*runner, executable], cwd=work, env=env, check=True, timeout=20)
                else:
                    print(f"{triple}: linked {mode}, execution skipped (no emulator)")
            for text, diagnostic in REJECTS:
                source.write_text(text + "\n", encoding="ascii")
                result = invoke("-fsyntax-only", source, capture_output=True, text=True)
                if result.returncode <= 0 or diagnostic not in result.stderr:
                    raise AssertionError(f"{triple}: {text}\nExpected {diagnostic!r}\n{result.stderr}")
            for ty, value in (("int", "0"), ("int", "-1"), ("unsigned long", "1UL << 62")):
                source.write_text(f"int main(void) {{ {ty} n = {value}; int a[n];"
                                  " return sizeof a == 0; }\n", encoding="ascii")
                invoke(source, "-o", executable, check=True)
                if runner is not None:
                    result = subprocess.run([*runner, executable], cwd=work, env=env,
                                            capture_output=True, timeout=20)
                    assert result.returncode == -signal.SIGTRAP, (triple, value, result)


if __name__ == "__main__":
    main()
