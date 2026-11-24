"""Check the native compiler's %Fault diagnostic overrides."""

from pathlib import Path
import sys
import tempfile

from integration import require, run


def test(binary, cwd):
    grammar = cwd / "faults.g"

    def check(text, status=1):
        grammar.write_text(text, encoding="ascii")
        result = run([binary, "--check", grammar], cwd=cwd, status=status)
        require(bool(result.stdout) == (status == 0), f"unexpected stdout: {result.stdout!r}")
        return result.stderr

    overlapping = (
        "%start entry\n"
        '%fault syntax.quote "not at the start" => "wrong prefix"\n'
        '%Fault syntax.quote "unterminated quoted" => "close the terminal"; // trailing comment\n'
        '%fault syntax.quote => "too late"\n'
        "entry: 'a\n"
    )
    require(check(overlapping).endswith(
        b":5:8: error [syntax.quote]: close the terminal\n"),
        "%Fault did not replace a Reader error while preserving its location and code")
    require(check('%Fault syntax.start => "choose a start"\nentry: \'a\';\n').endswith(
        b":1:1: error [syntax.start]: choose a start\n"),
        "%Fault did not handle an end-of-read check")
    require(check("entry: 'a\n" + overlapping).endswith(
        b":1:8: error [syntax.quote]: unterminated quoted terminal\n"),
        "a declaration after the failure should not be read")
    require(check('%Fault syntax.nul => "masked"\n%start entry\nentry: \'a\';\0\n').endswith(
        b":3:12: error [syntax.nul]: embedded NUL byte in grammar source\n"),
        "a declaration must not override pre-parse rejection of the source")

    warning = check(
        "%start entry\n"
        r'%Fault unsupported-directive "%layout" => "quoted \"value\"; C:\\path"' "\n"
        "%layout custom\n"
        "entry: 'a';\n",
        status=0,
    )
    require(warning.endswith(b':3:1: warning [unsupported-directive]: quoted "value"; C:\\path\n'),
            f"%Fault changed the warning's status, metadata or escaped message: {warning!r}")

    for rest, reason in (
        ("", b"expected a diagnostic code"),
        ("syntax.quote =>", b"expected a double-quoted string"),
        ('syntax.quote "unterminated', b"unterminated quoted string"),
        ('syntax.quote => "unterminated', b"unterminated quoted string"),
        (r'syntax.quote => "bad\q"', b"expected a quoted-string escape"),
        ('syntax.quote => "ok" extra', b"unexpected text after replacement"),
    ):
        diagnostic = check(
            "%start entry\n"
            '%fault fault-syntax => "masked"\n'
            f"%Fault {rest}\n"
            "entry: 'a';\n")
        require(b":3:1: error [fault-syntax]:" in diagnostic and reason in diagnostic
                and b"[syntax.quote]" not in diagnostic and b"masked" not in diagnostic,
                f"wrong diagnosis for malformed %Fault {rest}: {diagnostic!r}")

    for rule, column in (
        ('entry %Fault syntax.quote => "wrong": \'a\' | \'b\';\n', 7),
        ('entry: \'a\' %fault syntax.quote => "wrong";\n', 12),
    ):
        diagnostic = check("%start entry\n" + rule)
        require(diagnostic.endswith(f":2:{column}: error [fault-syntax]: ".encode()
                                    + b"%fault must be declared at grammar scope\n")
                and diagnostic.count(b"[fault-syntax]") == 1,
                f"expected one source-located scope error: {diagnostic!r}")

    prologue = (
        "%{\n"
        '%fault syntax.quote "nonmatching" => "wrong"\n'
        '%Fault syntax.quote => "close the prologue fallback terminal"\n'
        "%}\n"
        "%start entry\n"
    )
    require(check(prologue + "entry: 'a\n").endswith(b"close the prologue fallback terminal\n"),
            "the reader lost a later fallback pattern after a syntax error")
    require(check(prologue + '%Fault syntax.quote => "top level wins"\nentry: \'a\n').endswith(
        b"top level wins\n"), "a prologue pattern displaced a top-level declaration")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="aurocks-faults-") as output:
        test(str(Path(sys.argv[1]).resolve()), Path(output))
