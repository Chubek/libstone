"""Black-box tests for the DSymspec compiler driver."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--frontend", required=True, type=Path)
    parser.add_argument("--scratch", required=True, type=Path)
    parser.add_argument("spec", type=Path)
    args = parser.parse_args()
    command = [
        sys.executable, "-B", str(args.compiler), "--frontend", str(args.frontend),
    ]

    with tempfile.TemporaryDirectory(prefix="compiler-test-", dir=args.scratch) as scratch:
        directory = Path(scratch)

        def compile_spec(source, output=None):
            output_args = ["-o", str(output)] if output is not None else []
            return subprocess.run(
                [*command, *output_args, str(source)],
                cwd=directory, check=False, capture_output=True, text=True,
            )

        outputs = [directory / name for name in ("first.i", "second.i")]
        for output in outputs:
            result = compile_spec(args.spec, output)
            if result.returncode:
                raise AssertionError(result.stderr)
        if not outputs[0].stat().st_size or outputs[0].read_bytes() != outputs[1].read_bytes():
            raise AssertionError("the same profile did not generate identical nonempty output")

        profile_dir = directory / "profiles"
        profile_dir.mkdir()
        outside = directory / "outside.dsymspec"
        outside.write_text(
            'local s = require("backend").new("outside")\n'
            's.target{ arch = "outside", addr_size = 16, op_size = 8, addr_def = 16,\n'
            '    align = 1, byte_order = "little", obj_format = "ELF32", abi = "test" }\n'
            's.encoder{ name = "one", source = "emit8(1);", max_bytes = 1 }\n'
            's.mnemonic{ name = "one" }\n'
            's.form{ mnemonic = "one", encoder = "one", ops = 0 }\n'
            's.syntax{ name = "default", comment = "#" }\nprint(s.emit())\n',
            encoding="utf-8",
        )
        result = compile_spec(outside)
        if result.returncode:
            raise AssertionError(f"minimal standalone profile did not compile: {result.stderr}")
        if not (directory / "outside.i").is_file():
            raise AssertionError("compiler did not name the output after the custom profile")
        bad = {
            "unclosed": 'local s = require("backend").new("bad")\ns.target{ arch = "bad"\n',
            "host_call": 'local s = require("backend").new("bad")\n'
                         's.target{ arch = "bad" }\nos.exit(0)\nprint(s.emit())\n',
            "escaping_include": 'assert(loadfile("../outside.dsymspec"))("outside")\n',
            "recursive_include": 'assert(loadfile("recursive_include.dsymspec"))("bad")\n',
        }
        for name, source in bad.items():
            path = profile_dir / (name + ".dsymspec")
            output = directory / (name + ".i")
            path.write_text(source, encoding="utf-8")
            result = compile_spec(path, output)
            if not result.returncode or not result.stderr.strip() or output.exists():
                raise AssertionError(f"{name}: expected a diagnostic and no output, got {result}")


if __name__ == "__main__":
    main()
