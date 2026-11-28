"""Native GCC/Clang consumer test, compiling only against an isolated install."""
from pathlib import Path
import os
import select
import subprocess
import sys
import tempfile


def run(args, *, data=None, status=0, stdin=None, stdout=subprocess.PIPE):
    result = subprocess.run(args, cwd=work, input=data, stdin=stdin, stdout=stdout,
                            stderr=subprocess.PIPE, text=True, env=env, timeout=60)
    if result.returncode != status:
        raise RuntimeError(f"{args}: exit {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result


cmake, build, cxx = sys.argv[1:]
# Do not permit local include or link search paths to satisfy installed headers.
env = dict(os.environ)
for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "LIBRARY_PATH"):
    env.pop(name, None)
with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    root = work / "installed headers"
    run([cmake, "--install", build, "--prefix", root])
    [header] = root.rglob("smtlib2/smtlib2.hpp")
    cc = [cxx, "-std=c++17", "-pedantic-errors", "-I", header.parent.parent]
    (work / "lib.cpp").write_text("#include <smtlib2/smtlib2.hpp>\n"
                                 "bool ok() { return !smtlib2::Session{}.exited(); }\n")
    assert "frontend.hpp" not in run([*cc, "-MM", "lib.cpp"]).stdout
    run([*cc, "-c", "lib.cpp", "-o", "lib.o"])
    (work / "app.cpp").write_text("#include <smtlib2/smtlib2.hpp>\n"
                                 "bool ok(); int main() {return !ok();}\n")
    run([*cc, "app.cpp", "lib.o", "-o", "app"])
    run(["./app"])
    for mode in ["CLI", "REPL"]:
        run([*cc, "-DSMTLIB2_" + mode, "-x", "c++", "-c", header, "-o", mode + ".o"])
        run([cxx, mode + ".o", "-o", mode])
        run([cxx, mode + ".o", "lib.o", "-o", mode + "-mixed"])
        run(["./" + mode + "-mixed", "--help"])
    conflict = run([*cc, "-DSMTLIB2_CLI", "-DSMTLIB2_REPL", "-x", "c++",
                    "-fsyntax-only", header], status=1)
    assert "Select only one" in conflict.stderr
    cli_help = run(["./CLI", "--help"], data="(bogus)").stdout
    assert "Usage:" in cli_help and "-h|--help" in cli_help
    for mode in ["CLI", "REPL"]:
        for options in [("--bad",), ("--help=1",), ("-hh",), ("-42",), ("--help", "--bad")]:
            invalid = run(["./" + mode, *options], status=2)
            assert not invalid.stdout and invalid.stderr
        # C stdio EOF alone cannot distinguish an input failure from a clean EOF.
        with open(os.devnull, "wb") as unreadable:
            failed = run(["./" + mode], stdin=unreadable, status=1)
            assert not failed.stdout and "input failed" in failed.stderr
        with open(os.devnull, "rb") as unwritable:
            failed = run(["./" + mode, "--help"], stdout=unwritable, status=1)
            assert "help output failed" in failed.stderr
            args = ["./CLI"] if mode == "CLI" else ["./REPL", "--no-prompt"]
            failed = run(args, data="(check-sat)", stdout=unwritable, status=1)
            assert "protocol output failed" in failed.stderr
            failed = run(args, data="(bogus)", stdout=unwritable, status=1)
            assert "protocol" in failed.stderr and "output failed" in failed.stderr
    assert "missing.smt2" in run(["./CLI", "missing.smt2"], status=1).stderr
    dangerous = "missing'\n\x1b.smt2"
    missing = run(["./CLI", dangerous], status=1)
    assert missing.stderr.splitlines() == [r"cannot open input file 'missing\'\x0A\x1B.smt2'"]
    (work / "input-directory").mkdir()
    bad_file = run(["./CLI", "input-directory"], status=1)
    assert not bad_file.stdout and "input-directory" in bad_file.stderr
    assert run(["./CLI"], data="; only a comment").stdout == ""
    assert run(["./CLI", "-"], data="(assert false)(check-sat)(exit)broken").stdout == "unsat\n"
    (work / "input file.smt2").write_text("(assert false)(check-sat)")
    assert run(["./CLI", "input file.smt2"], data="(check-sat)").stdout == "unsat\n"
    assert run(["./CLI", "input file.smt2", "-"], status=2).stderr
    (work / "-42").write_text("(check-sat)")
    assert run(["./CLI", "--", "-42"]).stdout == "sat\n"
    (work / "--help").write_text("(assert false)(check-sat)")
    assert run(["./CLI", "--", "--help"]).stdout == "unsat\n"
    assert run(["./CLI", "--", "-"], data="(check-sat)").stdout == "sat\n"
    cli_error = run(["./CLI"], data="(bogus)(check-sat)", status=1)
    assert cli_error.stdout.startswith("(error ") and cli_error.stdout.count("\n") == 1
    assert not cli_error.stderr
    assert run(["./CLI"], data=("(set-logic QF_BV)(declare-const x (_ BitVec 17))"
                                "(assert (= x x))(check-sat)")).stdout == "unknown\n"
    assert "Usage:" in run(["./REPL", "-h"]).stdout
    for options in [("--no-prompt=false",), ("--no-prompt", "--no-prompt"), ("--", "FILE")]:
        assert run(["./REPL", *options], status=2).stderr
    assert run(["./REPL"], data=":quit").stderr.startswith("smtlib2> ")
    repl = run(["./REPL", "--no-prompt"], data=(
        "; :quit\n:help\n:missing\n(declare-const x Bool)(assert\n x)\n"
        "(echo \"\n:quit\") (assert (not x))(check-sat)\n"
        ":qu'it'\n(check-sat)"))
    assert repl.stderr == "" and repl.stdout.startswith(":help")
    assert "unknown subcommand" in repl.stdout and repl.stdout.endswith("unsat\n")
    malformed = run(["./REPL", "--no-prompt"], data=":quit'\n(check-sat)")
    assert "unterminated directive quote" in malformed.stdout and malformed.stdout.endswith("sat\n")
    assert run(["./REPL", "--no-prompt"], data="(bogus)(check-sat)").stdout.endswith("sat\n")
    assert run(["./REPL", "--no-prompt"], data="(assert", status=1).stdout.startswith("(error ")
    assert run(["./REPL", "--no-prompt"], data=")\n(check-sat)\n:help", status=1
               ).stdout.count("\n") == 1
    assert run(["./REPL", "--no-prompt"], data=":quit --help\r\n(check-sat):quit\r(check-sat)"
                ).stdout.endswith("sat\n")
    invalid_directive = run(["./REPL", "--no-prompt"], data=":quit\0\n(check-sat):quit").stdout
    assert invalid_directive.startswith("(error ") and invalid_directive.endswith("sat\n")
    assert run(["./REPL", "--no-prompt"], data="(exit)\n:help").stdout == ""
    assert run(["./REPL", "--no-prompt"], data="; comment at EOF").stdout == ""
    assert run(["./REPL", "--no-prompt"], data=":" + "x" * 65536,
               status=1).stderr.startswith("directive byte limit exceeded")

    # Keep stdin open until a response arrives to exercise actual streaming.
    for args in [["./CLI"], ["./REPL", "--no-prompt"]]:
        with subprocess.Popen(args, cwd=work, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, env=env) as process:
            try:
                # A complete command must not wait for a newline or EOF.
                process.stdin.write(b"; :quit\r\n(check-sat)")
                process.stdin.flush()
                assert select.select([process.stdout], [], [], 10)[0], f"{args}: no live response"
                assert os.read(process.stdout.fileno(), 4) == b"sat\n"
                stdout, stderr = process.communicate(b"(exit)", timeout=10)
                assert process.returncode == 0 and not stdout and not stderr
            finally:
                if process.returncode is None:
                    process.kill()
