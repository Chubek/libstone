#!/usr/bin/env python3
"""Exercise bundled floating text conversions through native Cephyr callers."""

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
    parser.add_argument("--qemu-amd64", type=Path)
    args = parser.parse_args()

    source = Path(__file__).resolve().parents[1] / "stdlib/tests/strtofp-cephyr.c"
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, args.qemu_amd64),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-strtofp-") as directory:
        work = Path(directory)
        env = {**os.environ, "XDG_CACHE_HOME": str(work / "cache")}
        for triple, hosts, emulator in targets:
            executable = work / triple
            for name, char_type, literal in (("strtold", "char", '"1"'),
                                             ("wcstold", "wchar_t", 'L"1"')):
                # Supply the real prototype explicitly, so this probes the
                # result ABI rather than just an absent header declaration.
                probe = work / "extended.c"
                probe.write_text(f"#include <stddef.h>\n"
                                 f"extern long double {name}(const {char_type} *, {char_type} **);\n"
                                 f"void f(long double *x) {{ *x = {name}({literal}, 0); }}\n")
                result = subprocess.run(
                    [args.cephyr.resolve(), "-std=c11", f"--target={triple}", "-c",
                     probe, "-o", work / "extended.o"], cwd=work, env=env,
                    capture_output=True, text=True, timeout=60,
                )
                result.check_returncode()
            subprocess.run(
                [args.cephyr.resolve(), "-std=c11", f"--target={triple}",
                 source, "-o", executable],
                cwd=work, env=env, check=True, timeout=180,
            )
            if host in hosts:
                runner = []
            elif emulator:
                runner = [emulator.resolve()]
            else:
                print(f"{triple}: statically linked; execution skipped (no emulator)")
                continue
            subprocess.run([*runner, executable], cwd=work, check=True, timeout=10)
            if triple.startswith("riscv64"):
                print(f"{triple}: native binary128 text conversions and binary32/64 declaration gates passed")
            else:
                print(f"{triple}: native floating text conversion tests passed")


if __name__ == "__main__":
    main()
