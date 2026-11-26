"""Check the runtime CLI without depending on native AST object details."""

import argparse
from pathlib import Path
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--tool", required=True, type=Path)
parser.add_argument("--schema", required=True, type=Path)
parser.add_argument("--work-dir", required=True, type=Path)
args = parser.parse_args()


def run(*words, body="", status=0, error=None):
    cmd = [str(args.tool), *map(str, words)]
    proc = subprocess.run(cmd, input=body, text=True, capture_output=True, cwd=args.work_dir)
    if proc.returncode != status or (error is not None and error not in proc.stderr):
        raise RuntimeError(f"{cmd!r}: wanted status {status}, error {error!r}; got {proc!r}")
    return proc


if "Usage:" not in run("--help").stdout:
    raise RuntimeError("missing help")
run("--nonsense", status=2, error="unknown option")
run(args.schema, status=2, error="expected SCHEMA and SCRIPT")
run("--check", args.schema)
run("--check", args.schema, "-", body="error('must not execute driver')")
run("--check", args.schema, "-", body="local = ", status=1, error="<stdin>")
run("--stdlib", args.work_dir / "does-not-exist", args.schema, "-", status=1, error="no std/")
run(args.schema, "-", "first", "--second", body="""
    assert(type(asdl) == "table")
    assert(arg[1] == "first" and arg[2] == "--second")
""")
run(args.schema, "-", body="error('driver failure')", status=1, error="driver failure")
