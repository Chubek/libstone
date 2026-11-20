"""Build target facts for strict-C99 unit harnesses using the real generator."""

from pathlib import Path

from archon.compiler import compile_file
from archon.irgen.backend import _render_abi
from archon.irgen.native_target import resolve_native_target


def write_native_abi(directory):
    root = Path(__file__).resolve().parents[1]
    db = compile_file(root / "stdspec" / "riscv" / "rv64im.arch")
    output = Path(directory) / "NativeABIData.c"
    output.write_text(_render_abi(resolve_native_target(db)["abi_facts"]), encoding="utf-8")
    return output
