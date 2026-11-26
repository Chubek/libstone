"""Check that asdlc stays native unless explicitly asked to delegate."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


tool = Path(sys.argv[1]).resolve()
source = Path(__file__).resolve().parent.parent
fixture = source / "tests/fixtures/calculator.asdl"
schema = b"module Sample { item = (int number) }\n"
perl = shutil.which("perl")


def run(command, *args, data=None, env=None):
    return subprocess.run([*map(str, command), *map(str, args)], cwd=work,
                          input=data, capture_output=True, env=env)


with tempfile.TemporaryDirectory(prefix="asdl-perl-", dir=tool.parent) as scratch:
    work = Path(scratch)
    fake_perl = work / "perl"
    fake_perl.write_text("#!/bin/sh\nprintf 'fake Perl invoked\\n' >&2\nexit 42\n", encoding="utf-8")
    fake_perl.chmod(0o755)
    env = dict(os.environ, PATH=scratch)

    native = run([tool], "--check", "--schema=-", "-", data=schema, env=env)
    assert native.returncode == 0 and b'"Sample"' in native.stdout, native
    assert not native.stderr, native
    arg_value = run([tool], "--schema", "--perl", "--check", "-", data=schema, env=env)
    assert arg_value.returncode == 0 and (work / "--perl").is_file(), arg_value
    (work / "--perl").write_bytes(schema)
    positional = run([tool], "--check", "--", "--perl", env=env)
    assert positional.returncode == 0, positional
    bad_native = run([tool], "--check", "-", data=b"invalid", env=env)
    assert bad_native.returncode == 1 and b"fake Perl" not in bad_native.stderr, bad_native
    bad_selector = run([tool], "--perl=true", "--version", env=env)
    assert bad_selector.returncode == 1 and b"fake Perl" not in bad_selector.stderr, bad_selector

    delegated = run([tool], "--version", "--perl", env=env)
    assert delegated.returncode == 42 and delegated.stderr == b"fake Perl invoked\n", delegated
    (work / "empty").mkdir()
    missing = run([tool], "--perl", "--version", env=dict(env, PATH=str(work / "empty")))
    assert missing.returncode == 1 and b"cannot run perl for --perl" in missing.stderr, missing

    if perl:
        oracle = [perl, source / "scripts/asdl.pl"]
        for args, data in [
            (["--version"], None),
            (["--check", "--schema=-", fixture], None),
            (["--check", "--schema=-", "-"], schema),
            (["--check", "-"], b"invalid"),
            (["--dot", "unsupported.dot", fixture], None),
        ]:
            expected = run(oracle, *args, data=data)
            actual = run([tool], "--perl", *args, data=data)
            assert (actual.returncode, actual.stdout) == (expected.returncode, expected.stdout), actual
            # Perl can include the path of the invoked script in fatal error suffixes.
            assert bool(actual.stderr) == bool(expected.stderr), actual
