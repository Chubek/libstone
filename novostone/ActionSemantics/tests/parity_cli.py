"""Exercise script-compatible CLI spellings without changing the public model."""
import argparse
import pathlib
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    args = parser.parse_args()
    args.cli = args.cli.resolve()
    args.work = args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    spec = args.work / "input.acsem"
    spec.write_text("language Parity { start E; syntax E = N(); rule N = give 42; }")
    output = args.work / "output"

    def run(options, code=0):
        result = subprocess.run([str(args.cli), *map(str, options)],
                                cwd=args.work, capture_output=True, timeout=45)
        assert result.returncode == code, (options, result.stdout, result.stderr)
        if code:
            assert result.stderr and not result.stdout
        else:
            assert not result.stderr
        return result.stdout

    for target in ("c", "python", "rust", "pascal", "latex"):
        expected = run(["--target", target, spec])
        assert run(["--emit", target, spec]) == expected
        assert run([spec, f"--emit={target}"]) == expected
        assert run(["--emit", target, "--", spec]) == expected
        run(["--output", output, spec, "--emit", target])
        assert output.read_bytes() == expected
        run([spec, f"--output={output}", f"--target={target}"])
        assert output.read_bytes() == expected
        extended = run(["--dialect", "2", "--define", "steps=123456",
                        "--target", target, spec])
        assert run([spec, f"--emit={target}", "--define=steps=123456",
                    "--dialect=2"]) == extended
    assert b"valid" in run([spec, "--check"])
    assert b"--emit" in run(["-h"])
    dashed = args.work / "-input.acsem"
    dashed.write_bytes(spec.read_bytes())
    assert b"valid" in run(["--check", "--", dashed.name])
    for options in (
        ["--emit"], ["--output"], ["--dialect"], ["--define"],
        [spec, "--emit=unknown"], [spec, "--emit=python", "--check"],
        [spec, "--check", "--output", output],
        [spec, "--emit=python", "-o", output, "--output", output],
        [spec, "--emit=python", "--dialect=3"],
        [spec, "--emit=python", "--unknown"],
        [spec, "--emit=python", spec],
        [spec, "--emit=python", "--define=steps=0", "-o", output],
        [spec, "--emit=python", "--dialect=1", "--define=steps=100", "-o", output],
    ):
        output.write_bytes(b"preserve this file\n")
        run(options, 2)
        assert output.read_bytes() == b"preserve this file\n"
    spec.write_text("not a specification")
    output.write_bytes(b"preserve invalid specification output\n")
    run([spec, "--emit=python", "--output", output], 2)
    assert output.read_bytes() == b"preserve invalid specification output\n"
    print("CLI aliases, option ordering, validation, and output preservation passed")


if __name__ == "__main__":
    main()
