#!/usr/bin/env python3
"""Exercise bundled floating scanners across the native and target-compiler ABIs."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cephyr", type=Path, required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--qemu-amd64", type=Path)
    parser.add_argument("--qemu-aarch64", type=Path)
    parser.add_argument("--qemu", type=Path, help="RV64 user-mode QEMU")
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    fixtures = root / "tests/fixtures/scanf_float"
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, args.qemu_amd64, []),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64, ["-march=armv8-a"]),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu, ["-march=rv64im", "-mabi=lp64", "-mno-relax"]),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-scanf-float-") as directory:
        work = Path(directory)
        env = dict(os.environ, XDG_CACHE_HOME=str(work / "cache"), LC_ALL="C")
        for triple, hosts, emulator, flags in targets:
            external = work / f"{triple}.o"
            executable = work / triple
            subprocess.run(
                [args.clang, f"--target={triple}", *flags, "-std=c11", "-O2",
                 "-Wall", "-Wextra", "-Werror", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-nostdinc", "-I", root / "stdlib/include",
                 "-c", fixtures / "external.c", "-o", external],
                cwd=work, env=env, check=True, timeout=60,
            )
            subprocess.run(
                [args.cephyr.resolve(), "-std=c11", f"--target={triple}",
                 fixtures / "native.c", external, "-o", executable],
                cwd=work, env=env, check=True, timeout=180,
            )
            if host in hosts and platform.system() == "Linux":
                runner = []
            elif emulator:
                runner = [emulator.resolve()]
            else:
                print(f"{triple}: linked; execution skipped (no matching host or emulator)")
                continue
            subprocess.run([*runner, executable], cwd=work, env=env,
                           input=b"1e+? 0x1p3\n", check=True, timeout=10)
            print(f"{triple}: mixed Cephyr/Clang floating scanf tests passed")


if __name__ == "__main__":
    main()
