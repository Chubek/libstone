"""Check string literal typing, concatenation, initializers, and storage."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


FIXTURE = Path(__file__).resolve().parent / "fixtures" / "c11_string_literals.c"
REJECTIONS = (
    ('const void *p = L"a" "" u8"b";', "cannot mix a UTF-8 prefix with a wide prefix"),
    ('const void *p = u"a" "" U"b";', "cannot mix different wide prefixes"),
    ('const void *p = U"a" "" L"b";', "cannot mix different wide prefixes"),
    (r'const void *p = u"\x10000";', "numeric escape out of range"),
    (r'const void *p = L"\x100000000";', "numeric escape out of range"),
    (r'const void *p = U"\ud800";', "invalid code point"),
    (r'const void *p = u8"\x100";', "numeric escape out of range"),
    ('char a[] = u"abc";', "encoding is incompatible with the array element type"),
    ('unsigned short a[] = "abc";', "encoding is incompatible with the array element type"),
    ('unsigned short a[2] = u"🙂x";', "string literal is too long"),
    ('int a[] = U"abc";', "encoding is incompatible with the array element type"),
    ('_Static_assert(0, u"wide diagnostic");', "static assertion failed: wide diagnostic"),
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu-aarch64")
    parser.add_argument("--qemu")
    args = parser.parse_args()
    host = platform.machine().lower()
    targets = (
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    )
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-strings-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        for triple, hosts, emulator in targets:
            driver = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]
            executable = work / triple
            subprocess.run([*driver, FIXTURE, "-o", executable], cwd=work, env=env, check=True)
            runner = [] if host in hosts else [emulator] if emulator else None
            if runner is not None:
                subprocess.run([*runner, executable], cwd=work, env=env, check=True)
            else:
                print(f"{triple}: linked; execution skipped (no emulator)")

            for index, (source, expected) in enumerate(REJECTIONS):
                path = work / f"bad-{index}.c"
                path.write_text(source + "\n", encoding="utf-8")
                result = subprocess.run(
                    [*driver, "-fsyntax-only", path],
                    cwd=work, env=env, capture_output=True, text=True,
                )
                if result.returncode <= 0 or expected not in result.stderr:
                    raise AssertionError(f"{triple}: expected {expected!r} for {source!r}\n"
                                         f"{result.stderr}")

            # Preprocessing preserves individual spellings and token boundaries.
            pp = subprocess.run([*driver, "-E", FIXTURE], cwd=work, env=env,
                                capture_output=True, text=True, check=True)
            if not re.search(r'L"\\x12"\s+L"3"\s+"\\0é"\s+L"\\777"', pp.stdout):
                raise AssertionError(f"{triple}: preprocessing changed string token spellings")
            path = work / "mixed.i"
            path.write_text('const void *p = L"a" "" U"b";\n', encoding="utf-8")
            result = subprocess.run([*driver, "-fsyntax-only", path], cwd=work, env=env,
                                    capture_output=True, text=True)
            if result.returncode <= 0 or "cannot mix different wide prefixes" not in result.stderr:
                raise AssertionError(f"{triple}: mixed prefixes were accepted in .i\n{result.stderr}")


if __name__ == "__main__":
    main()
