#!/usr/bin/env python3
"""Link native C callers to the bundled §7.6 runtime on supported targets."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cephyr", type=Path, required=True)
    parser.add_argument("--qemu-amd64", type=Path)
    parser.add_argument("--qemu-aarch64", type=Path)
    parser.add_argument("--qemu", type=Path, help="Unused: RV64IM soft-float has no fenv.h")
    args = parser.parse_args()

    source = Path(__file__).resolve().parents[1] / "stdlib/tests/fenv-cephyr.c"
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, args.qemu_amd64),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-fenv-") as directory:
        work = Path(directory)
        env = dict(os.environ, XDG_CACHE_HOME=str(work / "cache"))
        env.pop("CEPHYR_CONFIG", None)
        driver = [args.cephyr.resolve(), "-std=c11"]
        for triple, hosts, emulator in targets:
            executable = work / triple
            subprocess.run([*driver, f"--target={triple}", source, "-o", executable],
                           cwd=work, env=env, check=True, timeout=90)
            if host in hosts:
                runner = []
            elif emulator:
                runner = [emulator.resolve()]
            else:
                print(f"{triple}: linked; execution skipped (no matching host or emulator)")
                continue
            subprocess.run([*runner, executable], cwd=work, check=True, timeout=10)
            print(f"{triple}: native fenv library calls passed")

        unsupported = work / "unsupported.c"
        unsupported.write_text("#include <fenv.h>\n", encoding="ascii")
        result = subprocess.run([*driver, "--target=riscv64-linux-gnu", "-c",
                                 unsupported, "-o", work / "unsupported.o"],
                                cwd=work, env=env, capture_output=True, text=True, timeout=10)
        assert result.returncode and "Cephyr libc fenv.h is supported only" in result.stderr, result
        assert not (work / "unsupported.o").exists()


if __name__ == "__main__":
    main()
