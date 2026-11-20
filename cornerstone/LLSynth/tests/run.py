#!/usr/bin/env python3
"""Run every LLSynth regression suite using only the Python standard library."""

from pathlib import Path
import shlex
import subprocess
import sys


def main():
    tests = Path(__file__).resolve().parent
    root = tests.parent
    suites = [tests / "cli.py", *sorted(tests.rglob("test_*.py"))]
    commands = [[sys.executable, str(suite)] for suite in suites]
    commands.append(["prove", "-r", str(tests)])
    failures = []
    for command in commands:
        print(f"\n$ {shlex.join(command)}", flush=True)
        try:
            result = subprocess.run(command, cwd=root, timeout=600)
            if result.returncode:
                failures.append(shlex.join(command))
        except (OSError, subprocess.TimeoutExpired) as error:
            print(error, file=sys.stderr)
            failures.append(shlex.join(command))
    if failures:
        print("\nFailed suites:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print(f"\nAll {len(commands)} suites passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
