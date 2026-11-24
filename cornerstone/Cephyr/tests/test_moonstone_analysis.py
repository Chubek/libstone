#!/usr/bin/env python3
"""Exercise the Cephyr/Moonstone integration through the public CLI."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap


def run(cephyr, enabled):
    env = dict(os.environ, LC_ALL="C")
    env.pop("CEPHYR_CONFIG", None)
    with tempfile.TemporaryDirectory(prefix="cephyr-moonstone-test-") as scratch:
        work = Path(scratch)
        env["XDG_CACHE_HOME"] = str(work / "cache")
        source = work / "input.c"
        common = [cephyr, "--target=x86_64-linux-gnu"]
        analysis = ["--moonstone-analyze"]

        def compile_c(text, options=("-fsyntax-only",), *, success=True):
            source.write_text(textwrap.dedent(text).rstrip("\n") + "\n", encoding="utf-8")
            result = subprocess.run(
                common + list(options) + [source],
                cwd=work, env=env, text=True, capture_output=True, check=False, timeout=20,
            )
            assert (result.returncode == 0) == success, (result.args, result.returncode, result.stderr)
            return result

        if not enabled:
            for stage in ("-fsyntax-only", "--emit-interface=sexp"):
                result = compile_c("int f(void) { return 1; }", (stage, *analysis), success=False)
                assert "built without Moonstone analysis support" in result.stderr, result.stderr
            return

        result = compile_c("int f(void) { return 1; }", ("-E", *analysis), success=False)
        assert "requires C lowering" in result.stderr, result.stderr

        report = compile_c("int f(void) { int x; return x; }",
                           ("--emit-interface=sexp", "--no-factir", *analysis))
        assert "(status lowered)" in report.stdout
        assert "cephyr.moonstone.chisel.uninitialized" in report.stdout

        c = """\
            int uninitialized(void) {
                int x;
                return x;
            }
            int joined(int branch) {
                int x;
                if (branch) x = 8;
                return x;
            }
            int initialized(int branch) {
                int x = 1;
                if (branch) x = 9;
                x = 4;
                return x;
            }
            int both(int branch) {
                int x;
                if (branch) x = 7; else x = 8;
                return x;
            }
            int loop(int iterations) {
                int x = 1;
                while (iterations) {
                    x = 7;
                    iterations = iterations - 1;
                }
                return x;
            }
            int dead(void) {
                int x;
                return 9;
                return x;
            }
            extern void escape(int *);
            int unknown(void) {
                int x;
                int y = x;
                escape(&x);
                return y;
            }
            int projection(void) {
                int x[2];
                x[1] = 3;
                return x[0];
            }
            int pure(void) { return 2 * 3 + 4 - 4; }
            long wide(void) { return 2000000000L * 3L; }
            long negative(void) { return -7L * 6L; }
            unsigned unsigned_result(void) { return 1U; }
            long synth_budget(void) { return 17L * 29L * 31L * 37L * 43L; }
            int overflowing(void) { return 2147483647 + 1; }
            long wide_overflow(void) { return 4611686018427387904L * 2L; }
            extern int external(void);
            int effect(void) { external(); return 3; }
            """
        ordinary = compile_c(c)
        assert "cephyr.moonstone." not in ordinary.stderr, ordinary.stderr
        result = compile_c(c, ("-fsyntax-only", *analysis))
        diagnostics = result.stderr
        found = [line for line in diagnostics.splitlines()
                 if "cephyr.moonstone.chisel.uninitialized" in line]
        assert len(found) == 2, diagnostics
        assert "in 'uninitialized'" in found[0] and "Heuristic:" in found[0], diagnostics
        assert "in 'joined'" in found[1] and "Heuristic:" in found[1], diagnostics
        # Diagnostics for rejected functions are not partially published.
        for name in ("unknown", "projection", "effect"):
            assert any(f"in '{name}'" in line and "Unknown:" in line and
                       "[cephyr.moonstone.chisel]" in line for line in diagnostics.splitlines()), diagnostics
            assert not any(f"in '{name}'" in line and "Verified:" in line
                           for line in diagnostics.splitlines()), diagnostics
        assert "in 'pure': Verified: typed Sequent and closed MIR return 6" in diagnostics, diagnostics
        assert "in 'pure': Verified: equivalent closed MIR return expression" in diagnostics, diagnostics
        assert "in 'wide': Verified: typed Sequent and closed MIR return 6000000000" in diagnostics, diagnostics
        assert "in 'wide': No independently validated cheaper expression" in diagnostics, diagnostics
        assert "in 'negative': Verified: typed Sequent and closed MIR return -42" in diagnostics, diagnostics
        assert "in 'unsigned_result': Unknown:" in diagnostics, diagnostics
        assert "in 'synth_budget': Budget exhausted: synthesis candidate limit" in diagnostics, diagnostics
        assert "in 'overflowing': Unknown: arithmetic would overflow" in diagnostics, diagnostics
        assert "in 'wide_overflow': Unknown: arithmetic would overflow" in diagnostics, diagnostics

        result = compile_c("int uninitialized(void) { int x; return x; }",
                           ("-fsyntax-only", *analysis, "-Werror"), success=False)
        assert "[cephyr.moonstone.chisel.uninitialized]" in result.stderr, result.stderr

        mapped = """\
            #line 42 "original.c"
            int mapped(void) {
                int value;
                return value;
            }
            """
        result = compile_c(mapped, ("-fsyntax-only", *analysis, "--diagnostics-format=sarif"))
        entries = json.loads(result.stderr)["runs"][0]["results"]
        site = next(entry for entry in entries if entry["ruleId"] == "cephyr.moonstone.chisel.uninitialized")
        physical = site["locations"][0]["physicalLocation"]
        assert physical["artifactLocation"]["uri"].endswith("original.c"), site
        assert physical["region"]["startLine"] == 44, site
        assert "startColumn" not in physical["region"], site
        assert site["level"] == "warning" and site["message"]["text"].startswith("Heuristic:"), site
        assert "fixes" not in site, site

        for opts, name in (((), "plain.mir"), (analysis, "analysis.mir")):
            compile_c(c, ("--emit-mir", "-o", name, *opts))
        assert (work / "plain.mir").read_bytes() == (work / "analysis.mir").read_bytes()

        bounded = "int too_long(void) { return " + " + ".join(["1"] * 40) + "; }"
        result = compile_c(bounded, ("-fsyntax-only", *analysis))
        assert "Budget exhausted: straight-line Sequent instruction limit" in result.stderr, result.stderr
        assert "Verified:" not in result.stderr, result.stderr

        bounded_cfg = "int cfg_limit(int x) {\n" + "if (x) x = 1;\n" * 65 + "return x;\n}"
        result = compile_c(bounded_cfg, ("-fsyntax-only", *analysis))
        assert "Budget exhausted: CFG block limit" in result.stderr, result.stderr
        assert "Verified:" not in result.stderr, result.stderr

        repeated = "\n".join(f"int f{i}(void) {{ return {i}; }}" for i in range(40))
        result = compile_c(repeated, ("-fsyntax-only", *analysis))
        findings = [line for line in result.stderr.splitlines() if "[cephyr.moonstone." in line]
        assert len(findings) == 33 and "[cephyr.moonstone.limit]" in findings[-1], result.stderr


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--disabled", action="store_true")
    args = parser.parse_args()
    run(args.cephyr.resolve(), not args.disabled)
