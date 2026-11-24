"""Check source validation and failures in generated parsers."""

from pathlib import Path
import sys
import tempfile

from integration import require, run


def test(binary, parsers, cwd):
    grammar = cwd / "faults.g"

    def check(declaration, *, backend="portable"):
        grammar.write_text(f"%start entry\n{declaration}\nentry: 'a';\n", encoding="ascii")
        return run([binary, "--check", "--backend", backend, grammar], cwd=cwd, status=1).stderr

    for text, code, fragment in (
        ('%Fault incomplete', b"fault-syntax", b"expected a double-quoted string"),
        ('%fault "!.', b"fault-syntax", b"unterminated quoted string"),
        ('%fault "\'a\'" => typo ""', b"fault-syntax", b"expected silent_patch"),
        ('%fault "([a-c]" => warn "bad"', b"fault-peg", b"expected ')'"),
        ('%fault "(!.)*" => silent_patch ""', b"fault-peg", b"repetition"),
        ('%fault "\'a\'" => error "bad" using int', b"fault-syntax", b"available C identifier"),
    ):
        diagnostic = check(text)
        require(b":2:1: error [" + code + b"]:" in diagnostic and fragment in diagnostic, diagnostic)

    grammar.write_text('%start entry\nentry: \'a\' %Fault "!." => error "end";\n', encoding="ascii")
    failure = run([binary, "--check", grammar], cwd=cwd, status=1)
    require(b"[fault-syntax]: %fault must be declared at grammar scope" in failure.stderr, failure.stderr)

    for parser in parsers:
        for text, stdout, diagnostic in (
            ("n;", b"ok 1 0\n", b""),
            ("n,", b"ok 1 1\n", b""),
            ("n", b"ok 1 1\n", b"patch at byte 1: inserted semicolon"),
            ("n?", b"1 0 1\n", b"info at byte 1: unexpected question mark"),
            ("?", b"0 0 1\n", b"info at byte 0: unexpected question mark"),
            ("n!", b"1 0 1\n", b"warning at byte 1: unexpected punctuation"),
            ("n~", b"1 0 1\n", b"error at byte 1: explicit runtime error"),
            ("n$", b"1 0 1\n", b"error at byte 1: explicit runtime error"),
            ("n,x", b"2 0 1\n", b""),
            ("n:x", b"2 0 1\n", b""),
            ("n:!", b"2 0 2\n", b"warning at byte 2: unexpected punctuation"),
            ("n:", b"ok 1 2\n", b"patch at byte 2: inserted semicolon"),
            ("n,,", b"1 0 0\n", b""),
            ("n#", b"1 0 1\n", b"fault patch made no progress"),
            ("n%", b"1 0 9\n", b"exceeded recovery limits"),
            ("n@", b"1 0 1\n", b"invalid diagnostic action"),
            ("tx", b"2 0 1\n", b"fault patch made no progress"),
        ):
            result = run([parser, text], cwd=cwd)
            require(result.stdout == stdout and (diagnostic in result.stderr if diagnostic else not result.stderr),
                    f"{parser} on {text!r}: {result.stdout!r}, {result.stderr!r}")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="aurocks-faults-") as output:
        test(str(Path(sys.argv[1]).resolve()), [str(Path(name).resolve()) for name in sys.argv[2:]],
             Path(output))
