#!/usr/bin/env python3
"""Native CLI outputs must be genuine artifacts, or fail without clobbering files."""
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tempfile

cli = str(Path(sys.argv[1]).resolve())
fixture = Path(sys.argv[2]).resolve()


def run(*args, env=None):
    return subprocess.run([cli, *map(str, args)], capture_output=True, text=True, env=env)


with tempfile.TemporaryDirectory(prefix="jiterati-cli-") as work:
    temp = Path(work)
    output = temp / "out"
    sentinel = b"preserve existing output\n"
    output.write_bytes(sentinel)
    # A known backend without a host assembler must not embed assembly as code.
    result = run("compile", "--target", "wasm", "--emit", "obj", fixture, "--out", output)
    assert result.returncode != 0 and output.read_bytes() == sentinel, result
    # No toolchain on PATH: no fabricated obj/exe output, regardless of target.
    env = {**os.environ, "PATH": str(temp), "TMPDIR": str(temp)}
    result = run("compile", "--target", "amd64", "--emit", "obj", fixture, "--out", output, env=env)
    assert result.returncode != 0 and output.read_bytes() == sentinel, result
    if platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64"):
        # Constant-only TAC is in the current assembler-ready lowering subset.
        constant = temp / "constant.terse"
        constant.write_text("terse module constant\nfn answer() -> i32 = 42\n")
        result = run("compile", "--target", "amd64", "--emit", "obj", constant, "--out", output)
        assert result.returncode == 0, result.stderr
        data = output.read_bytes()
        assert data[:6] == b"\x7fELF\x02\x01" and struct.unpack_from("<H", data, 16)[0] == 1
        # A normal callable is not a process entry point. Never guess _start.
        output.write_bytes(sentinel)
        result = run("compile", "--target", "amd64", "--emit", "exe", constant, "--out", output)
        assert result.returncode != 0 and "_start" in result.stderr and output.read_bytes() == sentinel
        # Distinct archived functions must not all execute the first function.
        source = temp / "two.terse"
        source.write_text("terse module two\nfn first() -> i32 = 7\nfn second() -> i32 = 42\n")
        bytecode = temp / "two.jbc"
        result = run("compile", "--target", "amd64", "--emit", "bc", source, "--out", bytecode)
        assert result.returncode == 0, result.stderr
        result = run("run", bytecode, "--exec", "--entry", "first")
        assert result.returncode == 0 and "first() = 7" in result.stdout, result
        result = run("run", bytecode, "--exec", "--entry", "second")
        assert result.returncode == 0 and "second() = 42" in result.stdout, result
print("native CLI artifacts passed")
