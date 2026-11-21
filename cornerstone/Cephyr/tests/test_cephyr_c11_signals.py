#!/usr/bin/env python3
"""Exercise the bundled Linux signal ABI through real static executables."""

import argparse
from pathlib import Path
import os
import platform
import resource
import selectors
import signal
import subprocess
import tempfile


def check_external_signal(command, work):
    with subprocess.Popen(
        [*command, "external"], cwd=work, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    ) as process, selectors.DefaultSelector() as selector:
        try:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=10) or os.read(process.stdout.fileno(), 6) != b"ready\n":
                raise RuntimeError(f"{command}: handler-ready handshake failed")
            process.send_signal(signal.SIGTERM)
            output, errors = process.communicate(timeout=10)
            if (process.returncode, output, errors) != (0, b"", b""):
                raise RuntimeError(
                    f"{command}: external signal failed: "
                    f"{process.returncode}, {output!r}, {errors!r}"
                )
        finally:
            if process.poll() is None:
                process.kill()


def run_signals(command, work):
    for arguments, status, stderr in [
        ([], 0, b""),
        (["default"], -signal.SIGTERM, b""),
        (["x"], 42, b""),
        (["return"], -signal.SIGABRT, b"A"),
        (["ignore"], -signal.SIGABRT, b""),
        (["fpe"], 40 + signal.SIGFPE, b""),
        (["l"], 40 + signal.SIGILL, b""),
        (["segv"], 40 + signal.SIGSEGV, b""),
    ]:
        result = subprocess.run(
            [*command, *arguments], cwd=work, capture_output=True, timeout=10,
        )
        # User-mode QEMU can describe the guest signal on stderr as well.
        if (
            result.returncode != status
            or result.stdout
            or result.stderr.split(b"qemu:", 1)[0] != stderr
        ):
            raise RuntimeError(
                f"{command} {arguments}: status {result.returncode} (expected {status}), "
                f"stdout={result.stdout!r}, stderr={result.stderr!r}"
            )
    check_external_signal(command, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cephyr", type=Path)
    parser.add_argument("--external-dir", type=Path, help="output of stdlib/tests/run.sh")
    parser.add_argument("--qemu-aarch64", type=Path)
    parser.add_argument("--qemu", type=Path, help="RV64 user-mode QEMU")
    args = parser.parse_args()
    if not args.cephyr and not args.external_dir:
        parser.error("supply --cephyr, --external-dir, or both")

    source = Path(__file__).resolve().parents[1] / "stdlib/tests/signals-cephyr.c"
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    ]
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="cephyr-signals-") as directory:
        work = Path(directory)
        env = {**os.environ, "XDG_CACHE_HOME": str(work / "cache")}
        for triple, hosts, emulator in targets:
            executables = []
            if args.cephyr:
                executable = work / triple
                subprocess.run(
                    [args.cephyr.resolve(), "-std=c11", f"--target={triple}",
                     source, "-o", executable],
                    cwd=work, env=env, check=True, timeout=120,
                )
                executables.append(executable)
            if args.external_dir:
                executables.append(args.external_dir.resolve() / triple / "test-signals-cephyr")
            if host in hosts:
                runner = []
            elif emulator:
                runner = [emulator.resolve()]
            else:
                print(f"{triple}: execution skipped (no matching host or emulator)")
                continue
            for executable in executables:
                run_signals([*runner, executable], work)
            print(f"{triple}: signal tests passed")


if __name__ == "__main__":
    main()
