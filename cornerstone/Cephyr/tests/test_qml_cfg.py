#!/usr/bin/env python3
"""Check opt-in QML passes across a loop, branch joins, and volatile accesses."""
import argparse
import json
import os
from pathlib import Path
import platform
import random
import re
import struct
import subprocess
import tempfile
import unittest


FIXTURES = Path(__file__).resolve().with_name("fixtures")
CFG = FIXTURES / "qml_cfg"
MAX_STEPS = 8
HALF = 1 << 63
MASK = (1 << 64) - 1
FIELDS = ("return", "final cell", "call count", "signed remainder", "unsigned control",
          *(f"callee input at iteration {i}" for i in range(MAX_STEPS)))
RECORD = struct.Struct(f"<{len(FIELDS)}Q")
PIPELINES = ((), ("canonicalize",), ("strength_reduce",), ("integer_reduce",),
             ("canonicalize", "strength_reduce", "integer_reduce"))


def inputs():
    # Include zero trips, both arms alone, and a transition through an equality
    # at index three. The cell and divisor may have their high bit set.
    boundaries = (
        (MASK, -HALF, MASK, 0),
        (MASK, -HALF, 0, 1),
        (HALF, HALF - 1, MASK, 1),
        (HALF + 1, -4, 0, MAX_STEPS),
        (HALF + 1, -3, 2, MAX_STEPS),
        (MASK, -1, 255, MAX_STEPS),
        (MASK, 0, 0, MAX_STEPS),
        (HALF - 1, 1, HALF, MAX_STEPS),
        (0, 4, 256, MAX_STEPS),
        (1, HALF - 1, 2, MAX_STEPS),
    )
    for i, case in enumerate(boundaries):
        yield f"boundary {i}", case
    for seed in (0x514D4C, 0xC0FFEE):
        rng = random.Random(seed)
        for i in range(32):
            # Bias half of the bounds towards the comparison threshold, so
            # random cases exercise joins instead of staying in just one arm.
            bound = rng.randrange(-5, MAX_STEPS + 1) if i % 2 else rng.getrandbits(64) - HALF
            yield (f"seed {seed:#x}, case {i}",
                   (rng.getrandbits(64), bound, rng.getrandbits(64), rng.randrange(MAX_STEPS + 1)))


def reference(case):
    initial, bound, divisor_bits, steps = case
    acc = cell = initial
    trace = []
    for index in range(steps):
        # Equality takes the remainder arm. Odd iterations keep the low byte
        # and attach a marker bit, irrespective of previous high bits.
        seen = (2 * cell) & MASK if index > bound + 3 else cell % HALF
        if index % 2:
            seen = 256 + seen % 256
        trace.append(seen)
        cell = (seen // 2) ^ (bound & MASK) ^ ((index << 60) & MASK)
        acc = ((acc ^ ((seen + cell) & MASK)) + cell) & MASK

    magnitude = abs(bound) % 256
    signed = -magnitude if bound < 0 else magnitude
    divisor = HALF if bound < 0 else divisor_bits | 1
    overlap = 128 if initial % 256 >= 128 and divisor_bits % 256 >= 128 else 0
    control = overlap ^ (initial % 255) ^ (initial % divisor)
    return (acc, cell, len(trace), signed & MASK, control, *trace, *([0] * (MAX_STEPS - len(trace))))


def initializer(case):
    cell, bound, bits, steps = case
    # Spell LONG_MIN without overflowing an intermediate signed literal.
    signed = f"(-{-bound - 1}L - 1L)" if bound < 0 else f"{bound}L"
    return f"{{{cell}UL, {signed}, {bits}UL, {steps}U}},\n"


def function(mir, name):
    body = mir.split(f"\nfunction {name} ", 1)[1]
    return re.split(r"\n(?:function |import |global |end$)", body, maxsplit=1)[0]


def cfg_effects(body):
    # Include block boundaries; discard SSA IDs on loads, stores and calls.
    # Retain branch targets, but not the SSA ID of a conditional.
    structure = r"(?m)^(block) |^(br) (.+)|^(cbr) \d+ (.+)|^((?:load|store)\w*) |^call \d+ (\w+) "
    return [match.groups() for match in re.finditer(structure, body)]


class QmlCfgTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory(prefix="cephyr-qml-cfg-")
        self.addCleanup(work.cleanup)
        self.work = Path(work.name)
        self.env = os.environ.copy()
        self.env.pop("CEPHYR_CONFIG", None)
        self.env["XDG_CACHE_HOME"] = str(self.work / "cache")
        # The opcode counts below measure QML transformations in isolation.
        (self.work / "Cephyr.toml").write_text("[blackstone]\nenabled = false\n", encoding="ascii")

    def run_command(self, command, timeout=60):
        result = subprocess.run(command, cwd=self.work, env=self.env, capture_output=True,
                                check=False, timeout=timeout)
        self.assertEqual(result.returncode, 0,
                         f"{command} exited {result.returncode}:\n{result.stderr.decode(errors='replace')}")
        return result.stdout

    def check_mir(self, baseline, selected, phases):
        before = function(baseline, "cfg_walk")
        after = function(selected, "cfg_walk")
        # This is a real control-flow/effect workload, not only algebraic
        # expressions. Check each block's branch targets and effect ordering.
        self.assertRegex(before, r"(?m)^slt ")
        self.assertRegex(before, r"(?m)^cbr ")
        self.assertRegex(before, r"(?m)^call \d+ cfg_observe ")
        self.assertEqual(cfg_effects(after), cfg_effects(before))

        def count(body, operation):
            return len(re.findall(rf"(?m)^{operation} ", body))

        # No SSA numbers or full optimized MIR snapshots are pinned.
        reductions = {"canonicalize": {"xor": 1, "add": 1},
                      "strength_reduce": {"mul": 1, "add": -1},
                      "integer_reduce": {"rem_u": 1, "and": 2, "or": 1}}
        for opcode in ("xor", "add", "mul", "rem_u", "and", "or"):
            self.assertGreater(count(before, opcode), 0, f"{opcode} in:\n{before}")
            expected = count(before, opcode) - sum(reductions[phase].get(opcode, 0)
                                                   for phase in phases)
            self.assertEqual(count(after, opcode), expected, after)
        if "strength_reduce" in phases:
            self.assertRegex(after, r"(?m)^add \d+ (\d+) \1$")
        if "integer_reduce" in phases:
            self.assertRegex(after, r"(?m)^trunc \d+ \d+ 63$")

        self.assertRegex(function(baseline, "cfg_signed_control"), r"(?m)^rem_s ")
        control = function(baseline, "cfg_unsigned_control")
        self.assertEqual(count(control, "and"), 3, control)
        self.assertEqual(count(control, "rem_u"), 2, control)
        for name in ("cfg_signed_control", "cfg_unsigned_control"):
            self.assertEqual(function(selected, name), function(baseline, name), name)

    def check_observations(self, actual, expected, cases, comparison):
        self.assertEqual(len(actual), len(cases), comparison)
        for (label, case), observed, truth in zip(cases, actual, expected):
            if observed != truth:
                for field, value, wanted in zip(FIELDS, observed, truth):
                    if value != wanted:
                        self.fail(f"{comparison}: {label}, input (cell, bound, divisor bits, steps) "
                                  f"{case}: {field} was {value:#018x}, expected {wanted:#018x}")

    def test_guarded_phase_exhaustion_emits_no_partial_mir(self):
        result = subprocess.run(
            (ARGS.cephyr, "--no-factir", "--target=x86_64-linux-gnu", "--qml-pass=integer_reduce",
             "--qml-param=integer_reduce.fuel=1", "--diagnostics-format=sarif",
             "--emit-mir", "-o", "-", CFG / "kernel.c"),
            cwd=self.work, env=self.env, capture_output=True, check=False, timeout=60)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, b"")
        diagnostics = json.loads(result.stderr)["runs"][0]["results"]
        self.assertEqual(len(diagnostics), 1, diagnostics)
        self.assertEqual(diagnostics[0]["ruleId"], "cephyr.qml.fuel")
        self.assertEqual(diagnostics[0]["level"], "error")
        self.assertRegex(diagnostics[0]["message"]["text"],
                         r"fuel exhausted in phase 'integer_reduce' at rule '[a-z0-9_]+'")

    def test_seeded_cfg_observations(self):
        cases = list(inputs())
        expected = [reference(case) for _, case in cases]
        (self.work / "inputs.inc").write_text(
            "".join(initializer(case) for _, case in cases), encoding="utf-8")
        targets = (
            ("x86_64-linux-gnu", {"amd64", "x86_64"}, ARGS.qemu_x86_64),
            ("aarch64-linux-gnu", {"arm64", "aarch64"}, ARGS.qemu_aarch64),
            ("riscv64-linux-gnu", {"riscv64"}, ARGS.qemu),
        )
        host = platform.machine().lower()
        for target, native, qemu in targets:
            cephyr = (ARGS.cephyr, "--no-factir", f"--target={target}", "-ffreestanding")
            with self.subTest(target=target):
                self.run_command((*cephyr, "-c", CFG / "driver.c", "-I", self.work, "-o", "driver.o"))
                self.run_command((*cephyr, "-c", FIXTURES / "qml_differential" / "start.S",
                                  "-o", "start.o"))
                self.run_command((*cephyr, "-c", CFG / "write_stdout.S", "-o", "write_stdout.o"))
                baseline_mir = self.run_command(
                    (*cephyr, "--emit-mir", "-o", "-", CFG / "kernel.c")).decode()
                baseline = None
                for phases in PIPELINES:
                    with self.subTest(pipeline="+".join(phases) or "baseline"):
                        flags = tuple(f"--qml-pass={phase}" for phase in phases)
                        if phases:
                            selected = self.run_command(
                                (*cephyr, *flags, "--emit-mir", "-o", "-", CFG / "kernel.c"))
                            self.check_mir(baseline_mir, selected.decode(), phases)
                        binary = self.work / "cfg-oracle"
                        self.run_command((*cephyr, *flags, "-nostdlib", "-static", CFG / "kernel.c",
                                          "driver.o", "start.o", "write_stdout.o", "-o", binary))
                        if not qemu and host not in native:
                            self.skipTest(f"built {target}, but QEMU was not provided")
                        output = self.run_command((qemu, binary) if qemu else (binary,), timeout=20)
                        self.assertEqual(len(output), len(cases) * RECORD.size, "observation tape size")
                        actual = list(RECORD.iter_unpack(output))
                        if not phases:
                            baseline = actual
                        if baseline is not None and phases:
                            self.check_observations(actual, baseline, cases, "vs baseline")
                        self.check_observations(actual, expected, cases, "vs Python reference")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=lambda path: str(Path(path).resolve()))
    parser.add_argument("--qemu", metavar="QEMU_RISCV64")
    parser.add_argument("--qemu-aarch64")
    parser.add_argument("--qemu-x86_64")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
