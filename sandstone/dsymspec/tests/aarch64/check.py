"""Compare generated A64 encoder bytes, and ELF relocations, with LLVM MC."""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def run(*cmd, assembly=None):
    return subprocess.run(cmd, input=assembly, capture_output=True, text=True, check=True).stdout


def reference(mc, readobj, obj, feature, assembly, reloc):
    opts = [mc, "--triple=aarch64-none-linux-gnu"]
    if feature:
        opts += ["--mattr=" + feature]
    if reloc:
        run(*opts, "--filetype=obj", "-o", str(obj), assembly=assembly + "\n")
        report = run(readobj, "--relocations", "--hex-dump=.text", str(obj))
        byte_line = re.search(r"^\s*0x00000000 ([0-9a-f]{8})\b", report, re.M)
        relocs = re.findall(r"\s+0x([0-9A-Fa-f]+) (R_AARCH64_\w+) (\S+) 0x([0-9A-Fa-f]+)", report)
        actual_relocs = " ".join(
            f"{kind}@{name}@{int(offset, 16)}@{int(addend, 16)}"
            for offset, kind, name, addend in relocs
        )
        assert byte_line is not None and actual_relocs == reloc, report
        return byte_line[1] + " " + actual_relocs
    result = run(*opts, "--show-encoding", assembly=assembly + "\n")
    words = re.findall(r"encoding: \[([^\]]+)\]", result)
    assert len(words) == 1, result
    return bytes(int(byte, 16) for byte in words[0].split(",")).hex()


def main():
    fixture_dir = Path(__file__).absolute().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", type=Path, help="directory containing aarch64.i")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--llvm-mc", default="llvm-mc")
    parser.add_argument("--llvm-readobj", default="llvm-readobj")
    parser.add_argument("--scratch", default=Path("build"), type=Path)
    args = parser.parse_args()

    args.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="a64-", dir=args.scratch) as temp:
        os.environ["TMPDIR"] = str(Path(temp).absolute())
        driver = str(Path(temp).absolute() / "encode")
        run(*shlex.split(args.cc), "-std=c11", "-pedantic-errors", "-Wall", "-Wextra",
            "-Werror", "-I" + str(args.generated), "-I" + str(fixture_dir.parent.parent),
            str(fixture_dir / "encode.c"), "-o", driver)
        print(run(driver, "@inventory").strip())
        stats = [0, 0, 0]
        errors = []
        for num, line in enumerate((fixture_dir / "vectors.txt").read_text().splitlines(), 1):
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            try:
                encoder, values, feature, assembly, reloc = (p.strip() for p in line.split("|"))
                actual = run(driver, encoder, *values.split()).strip()
                if assembly == "reject":
                    expected = "rejected"
                    stats[1] += 1
                else:
                    expected = reference(args.llvm_mc, args.llvm_readobj, Path(temp) / "ref.o",
                                         feature, assembly, reloc)
                    stats[2 if reloc else 0] += 1
                assert actual == expected, f"expected {expected}; got {actual}"
            except (AssertionError, subprocess.CalledProcessError) as exc:
                errors.append(f"vectors.txt:{num}: {line}\n{exc}\n"
                              f"{getattr(exc, 'stderr', '')}")
        if errors:
            raise AssertionError("\n".join(errors))
        print(f"{stats[0]} encodings, {stats[1]} rejections, {stats[2]} relocations")


if __name__ == "__main__":
    main()
