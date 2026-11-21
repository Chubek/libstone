"""Exercise C11 character constants and static assertions in member lists."""

import argparse
import os
from pathlib import Path
import platform
import subprocess
import tempfile


FIXTURES = Path(__file__).resolve().parent / "fixtures"
CHARACTERS = FIXTURES / "c11_character_constants.c"
RECORD_ASSERTIONS = FIXTURES / "c11_record_static_assertions.c"


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
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-frontend-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env["LC_ALL"] = "C"
        env["XDG_CACHE_HOME"] = str(work / "cache")
        for triple, hosts, emulator in targets:
            compile_c = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]
            for fixture in (CHARACTERS, RECORD_ASSERTIONS):
                executable = work / f"{triple}-{fixture.stem}"
                subprocess.run([*compile_c, fixture, "-o", executable],
                               cwd=work, env=env, check=True)
                runner = [] if host in hosts else [emulator] if emulator else None
                if runner is None:
                    print(f"{triple}: {fixture.stem} linked; execution skipped (no emulator)")
                else:
                    subprocess.run([*runner, executable], cwd=work, env=env, check=True)

            result = subprocess.run(
                [*compile_c, "-fsyntax-only", "-DFAIL_ASSERTION", RECORD_ASSERTIONS],
                cwd=work, env=env, capture_output=True, text=True,
            )
            if result.returncode <= 0 or "static assertion failed: record assertion diagnostic" not in result.stderr:
                raise AssertionError(f"{triple}: missing failed member assertion diagnostic\n{result.stderr}")


if __name__ == "__main__":
    main()
