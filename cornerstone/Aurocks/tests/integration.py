"""Generate, compile, and run Aurocks parsers with the native CLI."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


FIXTURES = Path(__file__).resolve().parent


def run(command, *, cwd, status=0):
    process = subprocess.run(command, cwd=cwd, capture_output=True, timeout=30)
    if process.returncode != status:
        raise AssertionError(
            f"{command!r}: expected status {status}, got {process.returncode}\n"
            f"stdout:\n{process.stdout.decode(errors='replace')}\n"
            f"stderr:\n{process.stderr.decode(errors='replace')}"
        )
    return process


def require(condition, reason):
    if not condition:
        raise AssertionError(reason)


def test(binary, cc, cwd):
    def cli(*args, status=0):
        return run([binary, *args], cwd=cwd, status=status)

    def compile_and_run(c_file, *args):
        parser = c_file.with_suffix(".exe" if os.name == "nt" else ".bin")
        run([*cc, "-std=c99", "-pedantic-errors", str(c_file), "-o", str(parser)], cwd=cwd)
        run([str(parser), *args], cwd=cwd)

    fixture = FIXTURES / "expression.g"
    expression = cli(fixture)
    require(expression.stdout, "default CLI produced no C source on stdout")
    native = cwd / "expression.c"
    native.write_bytes(expression.stdout)
    compile_and_run(native)

    json = cwd / "json.c"
    require(not cli("-o", json, FIXTURES / "json.g").stdout, "-o leaked C onto stdout")
    compile_and_run(json)

    # Both entry-point forms must emit a callable symbol, not just accept the flag.
    named = cwd / "named.g"
    named.write_text(
        "%{\n#include <string.h>\nstatic int marker;\n%}\n"
        "%start token\n"
        "token: 'a' { /* $99 is a comment. */\n"
        "    if (strcmp(\"$2\", \"\\x24\" \"2\") == 0 && *$TOKEN == 'a') { $$ = &marker; }\n"
        "    free($TOKEN);\n"
        "};\n"
        "%%\n"
        "int main(void) { return !custom_parser(\"a\") || !yyparse(\"a\") || yyparse(\"b\"); }\n",
        encoding="ascii",
    )
    custom = cwd / "named.c"
    custom.write_bytes(cli(named, "-e", "custom_parser").stdout)
    compile_and_run(custom)
    require(cli("--entrypoint=custom_parser", "--output=-", named).stdout == custom.read_bytes(),
            "long/equal option spelling differs from short spelling")
    require(cli("--no-strpool", fixture).stdout == expression.stdout
            and not (cwd / "AUROCKS_STRPOOL").exists(),
            "legacy --no-strpool changed standalone output or produced a sidecar")

    help_text = cli("--help").stdout
    for flag in (b"--entrypoint", b"--output", b"--check", b"--backend", b"--algorithm",
                 b"--disambiguator", b"--lua", b"--version", b"--no-strpool"):
        require(flag in help_text, f"help omits {flag!r}")
    version = cli("--version").stdout
    require(version == cli("-V").stdout and re.match(rb"aurocks \d+\.\d+", version),
            f"unexpected version output: {version!r}")
    cli(status=2)
    cli("--bogus", named, status=2)
    cli("--entrypoint", "--check", status=2)
    cli(named, fixture, status=2)
    cli(cwd / "missing.g", status=1)
    cli("--lua", cwd / "missing.lua", named, status=1)
    cli("-o", cwd / "missing" / "parser.c", named, status=1)
    cli("-e", "not-a-c-name", named, status=1)
    for retired, migration in (
        ("--strpool", b"does not emit string pools"),
        ("--poolfile=output.pool", b"does not emit string pools"),
        ("--liblr-helper=old-helper", b"--backend liblr selects grammar analysis"),
    ):
        result = cli(retired, fixture, status=2)
        require(migration in result.stderr and not result.stdout,
                f"{retired} did not give a migration diagnostic")

    preserved = cwd / "preserved.c"
    preserved.write_text("keep\n", encoding="ascii")
    analysis = cli("--check", "-o", preserved, fixture).stdout
    require(re.fullmatch(
        rb"backend=portable algorithm=earley disambiguator=first states=\d+ conflicts=\d+\n",
        analysis), f"unexpected --check report: {analysis!r}")
    require(preserved.read_bytes() == b"keep\n", "--check changed the output file")
    for option in ("--backend", "--algorithm", "--disambiguator"):
        invalid_flag = cli("--check", option, "not-a-mode", fixture, status=1)
        require(not invalid_flag.stdout and b"error" in invalid_flag.stderr,
                f"invalid {option} did not fail with an error")

    settings = cwd / "settings.g"
    settings.write_text(
        "%start token\n"
        "%backend liblr\n"
        "%algorithm lalr1\n"
        "%disambiguator error\n"
        "%expect 0\n"
        "token: 'a';\n",
        encoding="ascii",
    )
    require(re.fullmatch(
        rb"backend=liblr algorithm=lalr1 disambiguator=error states=[1-9]\d* conflicts=0\n",
        cli("--check", settings).stdout), "grammar options did not reach liblr analysis")
    require(cli("--check", "--algorithm=slr1", settings).stdout.startswith(
        b"backend=liblr algorithm=slr1 disambiguator=error "), "CLI algorithm did not override grammar")
    require(cli("--check", "--backend=portable", "--algorithm=earley", "--disambiguator=first",
                settings).stdout.startswith(b"backend=portable algorithm=earley disambiguator=first "),
            "CLI backend or disambiguator did not override grammar")

    ambiguous = cwd / "ambiguous.g"
    ambiguous.write_text(
        "%{\nstatic int left, right, actions;\n%}\n"
        "%start entry\n"
        "entry: lhs { $$ = $1; } | rhs { $$ = $1; };\n"
        "lhs: 'a' { ++actions; $$ = &left; };\n"
        "rhs: 'a' { ++actions; $$ = &right; };\n"
        "%%\n"
        "int main(int argc, char **argv) {\n"
        "    void *parsed = yyparse(\"a\");\n"
        "    if (argc == 1) return parsed != &left || actions != 1;\n"
        "    if (argv[1][0] == 's') return parsed != &right || actions != 1;\n"
        "    return parsed != NULL || actions != 0;\n"
        "}\n",
        encoding="ascii",
    )
    selected = cwd / "first.c"
    selected.write_bytes(cli("--backend", "liblr", ambiguous).stdout)
    compile_and_run(selected)
    rejected = cwd / "error.c"
    rejected.write_bytes(cli("--disambiguator", "error", ambiguous).stdout)
    compile_and_run(rejected, "reject")

    scored = cwd / "scored.g"
    scored.write_bytes(b"%disambiguator lua\n" + ambiguous.read_bytes())
    cli("--check", scored, status=1)
    score_script = cwd / "score.lua"
    score_script.write_text(
        "assert(laurocks.production(2).lhs == 'entry')\n"
        "laurocks.set_rule_priority(2, 50)\n"
        "laurocks.warn('applied production scores')\n",
        encoding="ascii",
    )
    scoring = cli("--check", "--lua", score_script, scored)
    require(b"disambiguator=priority " in scoring.stdout
            and b"warning [lua-warning]: applied production scores" in scoring.stderr,
            "Lua scores or warnings did not reach the compiler result")
    scored_source = cwd / "scored.c"
    scored_source.write_bytes(cli("--lua", score_script, scored).stdout)
    compile_and_run(scored_source, "scored")

    rewriting = cwd / "rewriting.g"
    rewriting.write_text(
        "%rewriter lua\n"
        "%{\nstatic int marker;\n%}\n"
        "%start unused\n"
        "unused: 'x';\n"
        "token: 'a' { $$ = &marker; };\n"
        "%%\n"
        "int main(void) { return !yyparse(\"b\") || yyparse(\"a\"); }\n",
        encoding="ascii",
    )
    cli(rewriting, status=1)
    script = cwd / "rewrite.lua"
    script.write_text(
        "assert(laurocks.get_start() == 'unused')\n"
        "laurocks.set_start('token')\n"
        "laurocks.replace_literal(2, 1, 'b')\n",
        encoding="ascii",
    )
    generated = cwd / "rewritten.c"
    generated.write_bytes(cli("--lua", script, rewriting).stdout)
    compile_and_run(generated)

    sandbox = cwd / "sandbox.lua"
    sandbox.write_text(
        "assert(io == nil and os == nil and debug == nil and package == nil)\n"
        "assert(dofile == nil and load == nil and loadfile == nil and require == nil)\n"
        "assert(pcall == nil and coroutine == nil and getmetatable == nil)\n"
        "assert(laurocks.production_count() > 0)\n",
        encoding="ascii",
    )
    cli("--check", "--lua", sandbox, fixture)
    for source, diagnostic in (
        ("while true do end\n", b"instruction budget exceeded"),
        ("local s = 'x'; for i = 1, 32 do s = s .. s end\n", b"memory or allocation budget exceeded"),
    ):
        sandbox.write_text(source, encoding="ascii")
        failure = cli("--check", "--lua", sandbox, fixture, status=1)
        require(b"[lua-extension]" in failure.stderr and diagnostic in failure.stderr,
                f"sandbox did not stop its over-budget script: {failure.stderr!r}")

    invalid = cwd / "invalid.g"
    for bad_grammar in (
        "%start entry\nentry: missing;\n",
        "%start entry\nentry: 'a' { $$ = $2; };\n",
        "%start entry\nentry: 'a' { $$ = $1;\n",
        "%start entry\n%backend not-a-mode\nentry: 'a';\n",
        "%start entry\n%fault incomplete\nentry: 'a';\n",
    ):
        invalid.write_text(bad_grammar, encoding="ascii")
        failure = cli("-o", preserved, invalid, status=1)
        require(not failure.stdout and re.search(rb"invalid\.g:\d+:\d+: error", failure.stderr),
                f"expected a source-located error, got: {failure.stderr!r}")
        require(preserved.read_bytes() == b"keep\n", "a failed generation changed the output file")

    invalid.write_text(
        '%start entry\n'
        '%fault undefined-nonterminal "another message" => "wrong prefix"\n'
        '%fault undefined-nonterminal "no productions" => "did you spell the rule correctly?"\n'
        '%fault undefined-nonterminal => "matched too late"\n'
        "entry: misspelled;\n",
        encoding="ascii",
    )
    failure = cli("--check", invalid, status=1)
    require(b"[undefined-nonterminal]: did you spell the rule correctly?" in failure.stderr,
            f"%fault did not customize the diagnostic: {failure.stderr!r}")
    invalid.write_text("%start entry\nentry: 'a' { $$ = $2; };\n", encoding="ascii")
    require(b"[invalid-action-reference]" in cli("--check", invalid, status=1).stderr,
            "--check accepted an invalid semantic action reference")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(f"usage: {sys.argv[0]} AUROCKS C_COMPILER [C_COMPILER_ARG ...]")
    binary = Path(sys.argv[1]).resolve()
    cc = sys.argv[2:]
    if os.path.dirname(cc[0]):
        cc[0] = str(Path(cc[0]).resolve())
    with tempfile.TemporaryDirectory(prefix="aurocks-integration-") as output:
        test(str(binary), cc, Path(output))


if __name__ == "__main__":
    main()
