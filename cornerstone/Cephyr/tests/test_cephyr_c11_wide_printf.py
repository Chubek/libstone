#!/usr/bin/env python3
"""Run generated wide printf callers against Cephyr's bundled libc."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cephyr", type=Path, required=True)
    parser.add_argument("--qemu-aarch64", type=Path)
    parser.add_argument("--qemu", type=Path, help="RV64 user-mode QEMU")
    args = parser.parse_args()

    source = Path(__file__).resolve().parents[1] / "stdlib/tests/wide-printf-cephyr.c"
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-wide-printf-") as directory:
        work = Path(directory)
        env = dict(os.environ, XDG_CACHE_HOME=str(work / "cache"))
        for triple, hosts, emulator in targets:
            executable = work / triple
            subprocess.run(
                [args.cephyr.resolve(), "-std=c11", f"--target={triple}",
                 source, "-o", executable],
                cwd=work, env=env, check=True, timeout=120,
            )
            if host in hosts:
                runner = []
            elif emulator:
                runner = [emulator.resolve()]
            else:
                print(f"{triple}: linked; execution skipped (no matching host or emulator)")
                continue
            result = subprocess.run([*runner, executable], cwd=work,
                                    capture_output=True, timeout=10)
            if (result.returncode, result.stdout, result.stderr) != (0, b"wide:-7!\n", b""):
                raise RuntimeError(
                    f"{triple}: exit {result.returncode}, "
                    f"stdout={result.stdout!r}, stderr={result.stderr!r}"
                )
            print(f"{triple}: wide printf tests passed")


if __name__ == "__main__":
    main()
