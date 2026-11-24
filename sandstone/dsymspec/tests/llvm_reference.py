"""Check the portable encoder test vectors against LLVM MC when available."""

import argparse
import ast
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("llvm_mc")
    parser.add_argument("vectors", type=Path)
    args = parser.parse_args()

    literal = r'("(?:\\.|[^"\\])*")'
    pattern = re.compile(
        r"^ENCODING\((\w+),\s*" + r",\s*".join([literal] * 5) + ",", re.M
    )
    source = args.vectors.read_text(encoding="utf-8")
    cases = pattern.findall(source)
    if not cases or len(cases) != source.count("\nENCODING("):
        raise AssertionError("could not read all reference test vectors")

    checked = 0
    for profile, encoder, triple, cpu, assembly, expected in cases:
        triple, cpu, assembly = map(ast.literal_eval, (triple, cpu, assembly))
        expected = ast.literal_eval("b" + expected)
        command = [args.llvm_mc, "--show-encoding", "--triple=" + triple]
        if cpu:
            command.append("--mcpu=" + cpu)
        result = subprocess.run(
            command, input=assembly + "\n", check=False, capture_output=True, text=True
        )
        if result.returncode and "unable to get target" in result.stderr.lower():
            print(f"Skip {triple}: LLVM target not registered")
            continue
        if result.returncode:
            raise AssertionError(f"{profile}/{encoder}: {result.stderr}")
        values = re.findall(r"encoding: \[(.*?)\]", result.stdout)
        actual = bytes(int(item, 16) for value in values for item in value.split(","))
        if actual != expected:
            raise AssertionError(
                f"{profile}: {assembly}: expected {expected.hex(' ')}, got {actual.hex(' ')}"
            )
        checked += 1

    print(f"Checked {checked} encoding vectors against LLVM MC")
    return 0 if checked else 77


if __name__ == "__main__":
    raise SystemExit(main())
