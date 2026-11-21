#!/usr/bin/env python3
"""Integration tests for Cephyr's verified, per-TU FactIR pipeline."""
import argparse
import concurrent.futures
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


SOURCE = """\
extern unsigned long witness(volatile unsigned long *);
unsigned long run(volatile unsigned long *p, unsigned long n) {
    unsigned long x = *p;
    *p = (x + n) + 0UL;
    return (0UL + witness(p)) + 7UL;
}
"""

PRESELECTION = """\
unsigned long power(unsigned long x) { return x * (8UL | (x & 0UL)); }
unsigned long single(unsigned long x) { return 1UL * x; }
unsigned long top(unsigned long x) { return (1UL << 63) * x; }
unsigned long nonpower(unsigned long x) { return x * 12UL; }
unsigned long partial(unsigned long x, unsigned long y) { return x * (y | 8UL); }
unsigned long phi(unsigned long x, unsigned long y) { return x * (y ? (8UL | (x & 0UL)) : 8UL); }
unsigned long phinear(unsigned long x, unsigned long y) { return x * (y ? 8UL : 12UL); }
unsigned char masked(unsigned long x) { return (unsigned char)(x & 127UL); }
unsigned char unmasked(unsigned long x) { return (unsigned char)(x & 511UL); }
signed char signedmasked(unsigned long x) { return (signed char)(x | ~127UL); }
signed char signednear(unsigned long x) { return (signed char)(x | ~255UL); }
unsigned char readbyte(volatile unsigned char *p) { return (unsigned char)*p; }
"""


class FactIRTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="cephyr-factir-test-")
        self.addCleanup(tmp.cleanup)
        self.path = Path(tmp.name)
        self.scratch = self.path / "scratch"
        self.scratch.mkdir(mode=0o700)
        self.env = {k: v for k, v in os.environ.items() if k != "CEPHYR_CONFIG"}
        self.env.update(TMPDIR=str(self.scratch), XDG_CACHE_HOME=str(self.path / "cache"))
        (self.path / "run.c").write_text(SOURCE, encoding="ascii")
        (self.path / "off.toml").write_text("[factir]\nenabled = false\n", encoding="ascii")

    def cli(self, *args, ok=True):
        result = subprocess.run([CEPHYR, "--no-cache", *args], cwd=self.path, env=self.env,
                                capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode == 0, ok, result.stdout + result.stderr)
        return result

    def mir(self, *args, ok=True):
        return self.cli("--emit-mir", "-o", "-", "run.c", *args, ok=ok)

    def test_disable_parity_and_artifact_validation(self):
        self.env["TMPDIR"] = str(self.path / "nonexistent")
        cli_off = self.mir("--no-factir")
        toml_off = self.mir("--config=off.toml")
        self.assertEqual(cli_off.stdout, toml_off.stdout)
        self.assertEqual(cli_off.stderr, toml_off.stderr)
        self.assertEqual(len(re.findall(r"(?m)^add ", cli_off.stdout)), 4)
        for mode in ("--no-factir", "--config=off.toml"):
            rejected = self.mir(mode, "--factir-smtlib=unused.smt2", ok=False)
            self.assertIn("require enabled FactIR", rejected.stderr)
            self.assertFalse((self.path / "unused.smt2").exists())
        self.assertEqual(list(self.scratch.iterdir()), [])
        self.env["TMPDIR"] = str(self.scratch)
        self.assertIn("distinct, new", self.mir("--factir-facts=run.c", ok=False).stderr)
        self.assertIn("distinct, new", self.mir("--factir-smtlib=-", ok=False).stderr)
        self.assertIn("distinct, new", self.mir("--factir-facts=same",
                                                 "--factir-smtlib=./same", ok=False).stderr)
        self.assertIn("exactly one C input", self.cli("-fsyntax-only", "run.c",
                      "--factir-smtlib=unused.smt2", ok=False).stderr)
        self.assertEqual((self.path / "run.c").read_text(), SOURCE)

    def test_proof_rewrite_and_dependencies(self):
        before = self.mir("--no-factir").stdout.splitlines()
        after = self.mir("--config=off.toml", "--factir",
                         "--factir-smtlib=obligation.smt2", "--factir-facts=relations").stdout.splitlines()
        self.assertEqual(len(before), len(after))
        differences = [(a.split(), b.split()) for a, b in zip(before, after) if a != b]
        self.assertEqual(len(differences), 2)
        for old, new in differences:
            self.assertEqual((old[0], new[0], old[1]), ("add", "copy", new[1]))
            self.assertIn(new[2], old[2:])
        proof_path = self.path / "obligation.smt2"
        proof = proof_path.read_text()
        self.assertIn("(_ BitVec 64)", proof)
        self.assertIn("(assert (distinct (bvadd x (_ bv0 64)) x))", proof)
        self.assertIn("Satie: unsat; applied to 2 instruction(s)", proof)
        if z3 := shutil.which("z3"):
            check = subprocess.run([z3, proof_path], check=True, text=True, capture_output=True, timeout=5)
            self.assertEqual(check.stdout.strip(), "unsat")
        facts = (self.path / "relations").read_text()
        for path in (proof_path, self.path / "relations"):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertIn('[function]\n(0, "run")', facts)
        self.assertIn('[direct_call]\n(0, "witness")', facts)
        sections = dict((name, set(body.strip().splitlines())) for name, body in
                        re.findall(r"(?ms)^\[(\w+)\]\n(.*?)(?=^\[|\Z)", facts))
        call_id = next(line.split()[1] for line in after if line.startswith("call "))
        result_id = next(line.split()[1] for line in after if line.startswith("ret "))
        transitive = f"(0, {call_id}, {result_id})"
        self.assertIn(transitive, sections["dependency"])
        self.assertNotIn(transitive, sections["flow"])
        self.assertIn(f"(0, {call_id})", sections["observable_input"])
        self.assertEqual(list(self.scratch.iterdir()), [])

    def test_rv64_stage_consumes_a_preference_without_any_rewrite(self):
        (self.path / "order.c").write_text(
            "unsigned long schedule_probe(void) { 17UL; return 37UL; }\n", encoding="ascii")
        args = ("--target=riscv64-linux-gnu", "-v", "-S", "-o", "-", "order.c")
        baseline = self.cli(*args, "--no-factir")
        transformed = self.cli(*args, "--factir-facts=rv.facts", "--factir-smtlib=rv.smt2")
        self.assertNotIn("cephyr.factir.schedule", baseline.stderr)
        self.assertRegex(transformed.stderr, r"scheduling/allocation received [1-9]\d* positive")
        self.assertIn("no applicable", (self.path / "rv.smt2").read_text())
        self.assertLess(baseline.stdout.index(", 17\n"), baseline.stdout.index(", 37\n"))
        self.assertLess(transformed.stdout.index(", 37\n"), transformed.stdout.index(", 17\n"))
        self.assertEqual(self.cli(*args, "--config=off.toml").stdout, baseline.stdout)

    def test_rv64_reuses_block_local_call_spill_slots(self):
        (self.path / "spill.c").write_text("""\
typedef unsigned long U;
extern U foreign(U, U, U, U, U, U, U, U, U, U);
U spill(volatile U *p, U (*step)(U), U x) {
    *p = step(1UL) + foreign(x, 1UL, 2UL, 3UL, 4UL, 5UL, 6UL, 7UL, 8UL, 9UL);
    *p = step(2UL) + foreign(x, 1UL, 2UL, 3UL, 4UL, 5UL, 6UL, 7UL, 8UL, 9UL);
    *p = step(3UL) + foreign(x, 1UL, 2UL, 3UL, 4UL, 5UL, 6UL, 7UL, 8UL, 9UL);
    *p = step(4UL) + foreign(x, 1UL, 2UL, 3UL, 4UL, 5UL, 6UL, 7UL, 8UL, 9UL);
    return *p;
}
U no_spill(void) { return 37UL; }
""", encoding="ascii")
        target = "--target=riscv64-linux-gnu"
        args = (target, "-S", "-o", "-", "spill.c")
        off = self.cli(*args, "--no-factir").stdout
        on = self.cli(*args, "--factir", "--factir-smtlib=spill.smt2").stdout
        self.assertIn("no applicable", (self.path / "spill.smt2").read_text())
        self.assertEqual(off[off.index("no_spill:"):], on[on.index("no_spill:"):])
        adjustment = r"\baddi (?:sp|x2), (?:sp|x2), -(\d+)\b"
        before = re.search(adjustment, off)
        after = re.search(adjustment, on)
        self.assertIsNotNone(before, off)
        self.assertIsNotNone(after, on)
        self.assertGreaterEqual(int(before[1]) - int(after[1]), 16)
        self.assertEqual(int(after[1]) % 16, 0)

        # Compile both sides separately with different compilers. The foreign
        # callee checks all ten arguments (including the stack arguments);
        # distinct results keep values live over the second call in each pair.
        clang, lld = shutil.which("clang"), shutil.which("ld.lld")
        runner = [] if platform.machine() == "riscv64" else [EMULATORS.get("riscv64")]
        if not clang or not lld or None in runner:
            return
        (self.path / "foreign.c").write_text("""\
typedef unsigned long U;
extern U spill(volatile U *, U (*)(U), U), no_spill(void);
static unsigned seen, calls;
static volatile U result;
static U step(U n) {
    if (n > 4 || result != (n > 1 ? 102 + (n - 1) * 11 : 0))
        seen |= 1u;
    if (n <= 4)
        seen |= 1u << n;
    return n * 11;
}
U foreign(U a, U b, U c, U d, U e, U f, U g, U h, U i, U j) {
    if (b != 1 || c != 2 || d != 3 || e != 4 || f != 5 ||
        g != 6 || h != 7 || i != 8 || j != 9)
        return 17;
    ++calls;
    return a + 103;
}
int check(void) {
    return spill(&result, step, ~0UL) != 146 || result != 146 ||
        seen != 30 || calls != 4 || no_spill() != 37;
}
""", encoding="ascii")
        (self.path / "start.s").write_text("""\
.text
.globl _start
_start:
    call check
    li a7, 93
    ecall
""", encoding="ascii")
        for name, assembly in (("baseline", off), ("optimized", on)):
            with self.subTest(mode=name):
                (self.path / f"{name}.s").write_text(assembly, encoding="ascii")
                subprocess.run([clang, target, "-march=rv64im", "-mabi=lp64", "-fuse-ld=" + lld,
                                "-ffreestanding", "-nostdlib", "-static", "-O2",
                                "-Wl,--no-relax,-e,_start", "start.s", "foreign.c", f"{name}.s",
                                "-o", name], cwd=self.path, check=True, timeout=30)
                subprocess.run([*runner, self.path / name], cwd=self.path,
                               check=True, timeout=10)

    def test_preselection_consumes_known_bits(self):
        (self.path / "known.c").write_text(PRESELECTION, encoding="ascii")
        before = self.cli("--emit-mir", "-o", "-", "known.c", "--no-factir")
        after = self.cli("--emit-mir", "-o", "-", "known.c", "--factir", "--factir-facts=bits.txt")
        self.assertEqual(before.stdout, after.stdout)  # Not a MIR text rewrite.
        self.assertIn("[known_bits_i64]", (self.path / "bits.txt").read_text())

        def function(asm, name):
            start = re.search(rf'^"?{name}"?:$', asm, re.M)
            self.assertIsNotNone(start, asm)
            return asm[start.end():].split(".size", 1)[0]

        runner_harness = """\
extern unsigned long power(unsigned long), single(unsigned long), top(unsigned long);
extern unsigned long nonpower(unsigned long), partial(unsigned long, unsigned long);
extern unsigned long phi(unsigned long, unsigned long), phinear(unsigned long, unsigned long);
extern unsigned char masked(unsigned long), unmasked(unsigned long);
extern unsigned char readbyte(volatile unsigned char *);
extern signed char signedmasked(unsigned long), signednear(unsigned long);
int main(void) {
    unsigned long inputs[] = {0, 1, 127, 128, 255, 256, 511, ~0UL, 1UL << 63};
    for (unsigned i = 0; i < sizeof(inputs)/sizeof(inputs[0]); ++i) {
        unsigned long x = inputs[i];
        volatile unsigned char byte = (unsigned char)x;
        if (power(x) != x * 8UL || single(x) != x || top(x) != (x << 63) ||
            nonpower(x) != x * 12UL || partial(x, 1) != x * 9UL ||
            phi(x, 0) != x * 8UL || phi(x, 1) != x * 8UL ||
            phinear(x, 0) != x * 12UL || phinear(x, 1) != x * 8UL ||
            masked(x) != (x & 127UL) || unmasked(x) != (x & 255UL) ||
            signedmasked(x) != (signed char)(x | ~127UL) ||
            signednear(x) != (signed char)x || readbyte(&byte) != byte)
            return 1;
    }
    return 0;
}
"""
        (self.path / "harness.c").write_text(runner_harness, encoding="ascii")
        for arch, multiply, shl3, extend, byte_load in (
            ("x86_64", r"\bimulq\b", r"\bshlq \$3,", r"\bshlq \$(?:32|56),", r"\bmovzbq\b"),
            ("aarch64", r"\bmul\b", r"\blsl x9, x9, #3\b", r"\b[us]bfx\b", r"\bldrb\b"),
            ("riscv64", r"\bmul\b", r"\bslli [^\n]+, 3\b", r"\bslli [^\n]+, (?:32|56)\b", r"\blbu\b"),
        ):
            with self.subTest(target=arch):
                args = (f"--target={arch}-linux-gnu", "-S", "-o", "-", "known.c")
                off = self.cli(*args, "--no-factir").stdout
                on = self.cli(*args, "--factir").stdout
                for name in ("power", "single", "top", "phi"):
                    self.assertRegex(function(off, name), multiply)
                    self.assertNotRegex(function(on, name), multiply)
                self.assertRegex(function(on, "power"), shl3)
                self.assertRegex(function(on, "phi"), shl3)
                for name in ("nonpower", "partial", "phinear"):
                    self.assertRegex(function(on, name), multiply)
                for name in ("masked", "signedmasked", "readbyte"):
                    self.assertRegex(function(off, name), extend)
                    self.assertNotRegex(function(on, name), extend)
                for name in ("unmasked", "signednear"):
                    self.assertRegex(function(on, name), extend)
                self.assertEqual(len(re.findall(byte_load, function(off, "readbyte"))), 1)
                self.assertEqual(len(re.findall(byte_load, function(on, "readbyte"))), 1)
                runner = [] if arch == platform.machine() else [EMULATORS.get(arch)]
                if None not in runner:
                    for flag in ("--factir", "--no-factir"):
                        self.cli(args[0], flag, "known.c", "harness.c", "-o", "preselection")
                        subprocess.run([*runner, self.path / "preselection"], cwd=self.path,
                                       check=True, timeout=10)

    def test_preselection_bit_vector_lemmas(self):
        if not (z3 := shutil.which("z3")):
            self.skipTest("optional independent SMT solver")
        formulas, expected = ["(set-logic QF_BV)", "(declare-fun x () (_ BitVec 64))"], []

        def counterexample(lhs, rhs, premise="true", result="unsat"):
            formulas.extend(("(push)", f"(assert (and {premise} (distinct {lhs} {rhs})))",
                             "(check-sat)", "(pop)"))
            expected.append(result)

        for width in range(1, 64):
            low = f"((_ extract {width-1} 0) x)"
            clear = f"(= ((_ extract 63 {width}) x) (_ bv0 {64-width}))"
            sign_and_high = f"((_ extract 63 {width-1}) x)"
            n = 65 - width
            fill = f"(or (= {sign_and_high} (_ bv0 {n})) (= {sign_and_high} (_ bv{(1<<n)-1} {n})))"
            counterexample(f"((_ zero_extend {64-width}) {low})", "x", clear)
            counterexample(f"((_ sign_extend {64-width}) {low})", "x", fill)
        for shift in range(64):
            counterexample(f"(bvmul x (_ bv{1<<shift} 64))", f"(bvshl x (_ bv{shift} 64))")
        counterexample("(bvmul x (_ bv12 64))", "(bvshl x (_ bv3 64))", result="sat")
        counterexample("((_ sign_extend 56) ((_ extract 7 0) x))", "x",
                       "(= ((_ extract 63 8) x) (_ bv0 56))", result="sat")
        result = subprocess.run([z3, "-in"], input="\n".join(formulas), text=True, check=True,
                                capture_output=True, timeout=10)
        self.assertEqual(result.stdout.splitlines(), expected)

    def test_scalar_stage_forwards_observable_integer_result(self):
        (self.path / "scalar.c").write_text(
            "unsigned long scalar_sum(unsigned long a, unsigned long b) { return a + b; }\n",
            encoding="ascii")
        (self.path / "check_scalar.c").write_text("""\
extern unsigned long scalar_sum(unsigned long, unsigned long);
int main(void) {
    return scalar_sum(~0UL, 4UL) != 3UL ||
        scalar_sum(1UL << 63, 71UL) != ((1UL << 63) + 71UL);
}
""", encoding="ascii")
        patterns = {
            "x86_64": (r"(?m)^\taddq %rcx, %rax\n\tmovq %rax, (\d+)\(%rsp\)\n",
                       r"\tmovq {}\(%rsp\), %rax\n"),
            "aarch64": (r"(?m)^\tadd x9, x9, x10\n\tstr x9, \[sp, #(\d+)\]\n",
                        r"\tldr x9, \[sp, #{}\]\n"),
        }
        for arch, (producer, reload) in patterns.items():
            with self.subTest(target=arch):
                target = f"--target={arch}-linux-gnu"
                args = (target, "-S", "-o", "-", "scalar.c")
                baseline = self.cli(*args, "--no-factir").stdout
                optimized = self.cli(*args, "--factir", f"--factir-smtlib={arch}.smt2").stdout
                original = re.search(producer, baseline)
                after = re.search(producer, optimized)
                self.assertIsNotNone(original, baseline)
                self.assertIsNotNone(after, optimized)
                self.assertEqual(original.group(1), after.group(1))
                redundant_load = reload.format(original.group(1))
                self.assertRegex(baseline[original.end():], "^" + redundant_load)
                self.assertNotRegex(optimized[after.end():], "^" + redundant_load)
                self.assertIn("no applicable", (self.path / f"{arch}.smt2").read_text())
                if arch == "x86_64":
                    # The home store and the same epilogue remain, without
                    # reloading the result already held in the ABI register.
                    self.assertRegex(baseline, r"(?m)^\tmovq %rax, (\d+)\(%rsp\)\n"
                                     r"\tmovq \1\(%rsp\), %rax\n\taddq \$\d+, %rsp\n\tret$")
                    self.assertRegex(optimized, r"(?m)^\tmovq %rax, \d+\(%rsp\)\n"
                                     r"\taddq \$\d+, %rsp\n\tret$")
                else:
                    self.assertRegex(baseline, r"(?m)^\tstr x9, \[sp, #(\d+)\]\n"
                                     r"\tldr x0, \[sp, #\1\]\n\tadd sp, sp, #\d+\n\tret x30$")
                    self.assertRegex(optimized, r"(?m)^\tstr x9, \[sp, #\d+\]\n"
                                     r"\tmov x0, x9\n\tadd sp, sp, #\d+\n\tret x30$")
                if arch == platform.machine() or EMULATORS.get(arch):
                    runner = [] if arch == platform.machine() else [EMULATORS[arch]]
                    for mode in ("--factir", "--no-factir"):
                        self.cli(target, mode, "scalar.c", "check_scalar.c", "-o", "scalar_test")
                        subprocess.run([*runner, self.path / "scalar_test"], cwd=self.path,
                                       check=True, timeout=10)

    def test_concurrent_inputs_and_create_only_publication(self):
        (self.path / "other.c").write_text("unsigned long other(void) { return 5UL; }\n", encoding="ascii")

        def compile_source(job):
            name, index = job
            return self.cli("--emit-mir", "-o", "-", name, f"--factir-facts={index}.facts",
                            f"--factir-smtlib={index}.smt2")

        jobs = [("run.c", 1), ("other.c", 2), ("run.c", 3), ("other.c", 4)]
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
            list(executor.map(compile_source, jobs))
        for name, i in jobs:
            facts = (self.path / f"{i}.facts").read_text()
            proof = (self.path / f"{i}.smt2").read_text()
            self.assertIn(f'(0, "{Path(name).stem}")', facts)
            self.assertEqual("witness" in facts, name == "run.c")
            self.assertEqual("Satie: unsat" in proof, name == "run.c")
        original = (self.path / "1.facts").read_bytes()
        self.mir("--factir-facts=1.facts", ok=False)
        self.assertEqual((self.path / "1.facts").read_bytes(), original)
        self.cli("--emit-mir", "run.c", "other.c")
        self.assertIn("\ncopy ", (self.path / "run.mir").read_text())
        self.assertNotIn("\ncopy ", (self.path / "other.mir").read_text())

        def contest(name):
            return subprocess.run([CEPHYR, "--no-cache", "--emit-mir", "-o", "-", name,
                                   "--factir-facts=contested"], cwd=self.path, env=self.env,
                                  text=True, capture_output=True, timeout=20)

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            contenders = list(executor.map(contest, ("run.c", "other.c")))
        self.assertEqual(sorted(p.returncode for p in contenders), [0, 1])
        winner = "run" if contenders[0].returncode == 0 else "other"
        self.assertIn(f'(0, "{winner}")', (self.path / "contested").read_text())
        self.assertEqual(list(self.scratch.iterdir()), [])

    def test_native_executable_preserves_bits_and_observable_effects(self):
        (self.path / "check.c").write_text("""\
#include <limits.h>
extern unsigned long run(volatile unsigned long *, unsigned long);
static unsigned called;
static unsigned long wanted;
unsigned long witness(volatile unsigned long *p) {
    if (*p != wanted) return 0;
    ++called;
    return *p;
}
int main(void) {
    const unsigned long cases[] = {0, 1, ULONG_MAX, ULONG_MAX - 2, 1UL << 63};
    for (unsigned i = 0; i < sizeof(cases)/sizeof(*cases); ++i) {
        volatile unsigned long value = cases[i];
        wanted = value + 5;
        if (run(&value, 5) != wanted + 7 || value != wanted || called != i + 1)
            return 1;
    }
    return 0;
}
""", encoding="ascii")
        host = platform.machine()
        targets = {arch: ([] if arch == host else [EMULATORS[arch]])
                   for arch in ("x86_64", "aarch64", "riscv64")
                   if arch == host or EMULATORS.get(arch)}
        if not targets:
            self.skipTest("matching native target or QEMU required")
        for arch, runner in targets.items():
            for flag in ("--no-factir", "--factir"):
                with self.subTest(target=arch, mode=flag):
                    self.cli(f"--target={arch}-linux-gnu", flag, "run.c", "check.c", "-o", "prog")
                    subprocess.run([*runner, self.path / "prog"], cwd=self.path, check=True, timeout=10)


    def test_emission_fallthrough_retains_both_paths_and_effects(self):
        (self.path / "edge.c").write_text("""\
unsigned long edge(volatile unsigned long *out, unsigned long n) {
    unsigned long x = *out;
    if (n & 1UL) {
        *out = x + 11UL;
        x = *out;
    } else {
        *out = x ^ 19UL;
        x = *out;
    }
    return x ^ n;
}
""", encoding="ascii")
        (self.path / "check_edge.c").write_text("""\
extern unsigned long edge(volatile unsigned long *, unsigned long);
int main(void) {
    const unsigned long inputs[] = {0UL, 2UL, 3UL, ~0UL, 1UL << 63};
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(*inputs); ++i) {
        unsigned long n = inputs[i];
        volatile unsigned long x = ~n;
        const unsigned long expected = n & 1UL ? x + 11UL : x ^ 19UL;
        if (edge(&x, n) != (expected ^ n) || x != expected)
            return 1;
    }
    return 0;
}
""", encoding="ascii")
        patterns = {
            "x86_64": r"(?m)^\tjmp \.Larchon_0_([123])$",
            "aarch64": r"(?m)^\tb \.Larchon_0_([123])$",
            "riscv64": r"(?m)^\tauipc x28, %pcrel_hi\(\.Larchon0_([123])\)$",
        }
        for arch, pattern in patterns.items():
            with self.subTest(target=arch):
                target = f"--target={arch}-linux-gnu"
                args = (target, "-S", "-o", "-", "edge.c")
                baseline = self.cli(*args, "--no-factir").stdout
                self.assertEqual(baseline, self.cli(*args, "--config=off.toml").stdout)
                optimized = self.cli(*args, "--factir").stdout
                # The yes edge 0 -> 1 and the unconditional edge 2 -> 3
                # fall through. The no edge 0 -> 2 and the edge 1 -> 3 do not.
                self.assertEqual(re.findall(pattern, baseline), ["2", "1", "3", "3"])
                self.assertEqual(re.findall(pattern, optimized), ["2", "3"])
                if arch != platform.machine() and not EMULATORS.get(arch):
                    continue
                runner = [] if arch == platform.machine() else [EMULATORS[arch]]
                for mode in ("--factir", "--no-factir"):
                    self.cli(target, mode, "edge.c", "check_edge.c", "-o", "edge_test")
                    subprocess.run([*runner, self.path / "edge_test"], cwd=self.path,
                                   check=True, timeout=10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu")
    parser.add_argument("--qemu-aarch64")
    args, remaining = parser.parse_known_args()
    CEPHYR = args.cephyr.resolve()
    EMULATORS = {"aarch64": args.qemu_aarch64, "riscv64": args.qemu}
    unittest.main(argv=[sys.argv[0], *remaining])
