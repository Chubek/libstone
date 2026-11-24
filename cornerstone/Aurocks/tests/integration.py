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

    # Each regex alternative has its own beginning and end assertions.
    anchors = cwd / "anchors.g"
    anchors.write_text(
        "%start entry\n"
        "entry: /a$|^b/ | /^a|^c/ '!';\n"
        "%%\n"
        "int main(void) {\n"
        "    const char *bad = \"b!\";\n"
        "    yyparse(\"a\"); if (yyparse_error()) return 1;\n"
        "    yyparse(\"b\"); if (yyparse_error()) return 2;\n"
        "    yyparse(\"c!\"); if (yyparse_error()) return 3;\n"
        "    yyparse(bad); if (yyparse_error() != bad + 1) return 4;\n"
        "    return 0;\n"
        "}\n",
        encoding="ascii",
    )
    anchors_c = cwd / "anchors.c"
    anchors_c.write_bytes(cli(anchors).stdout)
    compile_and_run(anchors_c)

    invalid_regex = cwd / "invalid-regex.g"
    for body in ("entry: /a{1025}/;\n", "%skip /a{1025}/\nentry: 'a';\n"):
        invalid_regex.write_text("%start entry\n" + body, encoding="ascii")
        for backend in ("portable", "liblr"):
            emit = cli("--backend", backend, invalid_regex, status=1)
            check = cli("--backend", backend, "--check", invalid_regex, status=1)
            require(b"[invalid-regex]" in emit.stderr and emit.stderr == check.stderr
                    and not emit.stdout and not check.stdout,
                    f"{backend} check and emission disagree on invalid regex")

    cycle = cwd / "cycle.g"
    cycle.write_text("%start entry\nentry: entry | 'a';\n", encoding="ascii")
    for backend in ("portable", "liblr", "libglr"):
        for flags in ((), ("--check",)):
            failure = cli("--backend", backend, *flags, cycle, status=1)
            require(b"[cyclic-forest]" in failure.stderr and not failure.stdout,
                    f"{backend} accepted zero-input recursion: {failure.stderr!r}")
    for name in ("aurocks_parse_glr_ex", "aurocks_parser_spec", "AUROCKS_REGEX"):
        failure = cli("--backend", "libglr", "--check", "-e", name, fixture, status=1)
        require(b"[invalid-entrypoint]" in failure.stderr,
                f"linked runtime name {name} was accepted as an entry point")

    signed = cwd / "signed.g"
    signed.write_text(
        "%start entry\n%expect +0\nentry: 'a' %priority +2;\n",
        encoding="ascii",
    )
    for backend in ("portable", "liblr", "libglr"):
        cli("--check", "--backend", backend, signed)
        cli("--backend", backend, signed)
    signed.write_text("%start entry\nentry: 'a' %priority +-2;\n", encoding="ascii")
    require(b"[invalid-priority]" in cli("--check", signed, status=1).stderr,
            "a priority with two signs was accepted")

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
    scored.write_bytes(b"%disambiguator lua\n" + ambiguous.read_bytes().replace(
        b"lhs { $$", b"lhs %dprec 5 { $$"))
    cli("--check", scored, status=1)
    score_script = cwd / "score.lua"
    score_script.write_text(
        "local rule = laurocks.production(1)\n"
        "assert(rule.lhs == 'entry' and rule.rhs_count == 1 and rule.directive_count == 1)\n"
        "local name, value = laurocks.production_directive(1, 1)\n"
        "assert(name == 'dprec' and value == '5')\n"
        "for i = 1, laurocks.production_count() do\n"
        "  local kind, symbol = laurocks.symbol(i, 1)\n"
        "  if kind == 'nonterminal' and symbol == 'rhs' then\n"
        "    laurocks.set_rule_priority(i, value == '5' and 50 or 0)\n"
        "  end\n"
        "end\n"
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

    for source, diagnostic in (
        ("laurocks.set_rule_priority(1, 50)", b"explicit %dprec"),
        ("laurocks.set_rule_priority(2, 1.5)", b"integer score expected"),
        ("local rule = laurocks.production(2)", b"did not assign any production scores"),
    ):
        score_script.write_text(source, encoding="ascii")
        failure = cli("--lua", score_script, scored, status=1)
        require(not failure.stdout and b"[lua-extension]" in failure.stderr
                and diagnostic in failure.stderr, f"bad scoring script accepted: {failure.stderr!r}")
    score_script.write_text("laurocks.set_rule_priority(2, 50)", encoding="ascii")
    failure = cli("--check", "--disambiguator", "first", "--lua", score_script, scored, status=1)
    require(b"rule scores require %disambiguator lua" in failure.stderr,
            "scoring script bypassed the explicit host policy")
    require(b"disambiguator=priority " in cli("--check", "--disambiguator", "lua", "--lua",
                                               score_script, ambiguous).stdout,
            "explicit host Lua scoring opt-in did not select priority")

    rewriting = cwd / "rewriting.g"
    rewriting.write_text(
        "%rewriter lua\n"
        "%{\nstatic int marker;\n%}\n"
        "%start unused\n"
        "unused: 'x';\n"
        "token: 'a' { $$ = &marker; };\n"
        "entry: unused { $$ = $1; };\n"
        "%%\n"
        "int main(void) { return !yyparse(\"b\") || yyparse(\"a\"); }\n",
        encoding="ascii",
    )
    cli(rewriting, status=1)
    script = cwd / "rewrite.lua"
    script.write_text(
        "assert(laurocks.get_start() == 'unused')\n"
        "laurocks.set_start('entry')\n"
        "assert(laurocks.get_start() == 'entry')\n"
        "laurocks.replace_literal(2, 1, 'b')\n"
        "laurocks.replace_nonterminal(3, 1, 'token')\n"
        "local kind, symbol = laurocks.symbol(3, 1)\n"
        "assert(kind == 'nonterminal' and symbol == 'token')\n",
        encoding="ascii",
    )
    generated = cwd / "rewritten.c"
    generated.write_bytes(cli("--lua", script, rewriting).stdout)
    compile_and_run(generated)

    for grammar, source, diagnostic in (
        (rewriting, "laurocks.replace_nonterminal(1, 1, 'token')", b"expected a nonterminal"),
        (rewriting, "laurocks.warn('staged warning'); laurocks.replace_nonterminal(3, 1, 'missing')",
         b"has no production"),
        (ambiguous, "laurocks.replace_nonterminal(1, 1, 'rhs')", b"require %rewriter lua"),
    ):
        script.write_text(source, encoding="ascii")
        failure = cli("-o", preserved, "--lua", script, grammar, status=1)
        require(not failure.stdout and b"[lua-extension]" in failure.stderr
                and diagnostic in failure.stderr and b"lua-warning" not in failure.stderr,
                f"bad rewrite accepted: {failure.stderr!r}")
        require(preserved.read_bytes() == b"keep\n", "a failed rewrite changed the output file")

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
    for source in (b"-- comment with an embedded NUL\x00", b" " * (64 * 1024 + 1)):
        sandbox.write_bytes(source)
        failure = cli("--check", "--lua", sandbox, fixture, status=1)
        require(b"Lua source must be text of at most 64 KiB with no NUL bytes" in failure.stderr,
                f"invalid Lua source passed validation: {failure.stderr!r}")

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
        '%fault "!." => error "only a runtime message"\n'
        "entry: misspelled;\n",
        encoding="ascii",
    )
    failure = cli("--check", invalid, status=1)
    require(b"[undefined-nonterminal]: no productions" in failure.stderr and
            b"only a runtime message" not in failure.stderr,
            f"%fault changed a build diagnostic: {failure.stderr!r}")
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
