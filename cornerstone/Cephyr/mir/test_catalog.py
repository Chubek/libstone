#!/usr/bin/env python3
"""Validate catalog compilation and differential-check the generated matcher."""

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from compile_catalog import compile_catalog, read_passes


CATALOG = Path(__file__).resolve().parent
MASK = (1 << 64) - 1
SIGN = 1 << 63
WORDS = (0, 1, 2, 7, 63, 64, 65, 127, 0xFFFFFFFF, SIGN - 1, SIGN, SIGN + 1, MASK - 1, MASK)
LITERALS = {"Zero": 0, "One": 1, "AllOnes": MASK}


def signed(word):
    return word - (1 << 64) if word & SIGN else word


def div_signed(a, b):
    a, b = signed(a), signed(b)
    magnitude = abs(a) // abs(b)
    return -magnitude if (a < 0) != (b < 0) else magnitude


ORACLE = {
    "add": lambda a, b: a + b,
    "sub": lambda a, b: a - b,
    "mul": lambda a, b: a * b,
    "and": lambda a, b: a & b,
    "or": lambda a, b: a | b,
    "xor": lambda a, b: a ^ b,
    "shl": lambda a, b: a << (b % 64),
    "lshr": lambda a, b: a >> (b % 64),
    "ashr": lambda a, b: signed(a) >> (b % 64),
    "eq": lambda a, b: int(a == b),
    "slt": lambda a, b: int(signed(a) < signed(b)),
    "ult": lambda a, b: int(a < b),
    "div_u": lambda a, b: a // b if b else MASK,
    "rem_u": lambda a, b: a % b if b else a,
    "div_s": lambda a, b: div_signed(a, b) if b else MASK,
    "rem_s": lambda a, b: signed(a) - div_signed(a, b) * signed(b) if b else a,
}


CPP_TEST = """\
#include "mir_catalog.hpp"
#include <iostream>
#include <string>

using namespace cephyr::mir::catalog;

static_assert(lowering_stages[0].kind == StageKind::VerifyGraph);
static_assert(lowering_stages[1].kind == StageKind::ProjectTypes);
static_assert(lowering_stages[2].kind == StageKind::LinearizeBlocks);
static_assert(lowering_stages[3].kind == StageKind::AssignWireIds);
static_assert(lowering_stages[4].kind == StageKind::EmitArchonMir);

constexpr std::uint64_t only(std::string_view name)
{
    for (std::size_t i = 0; i < passes.size(); ++i)
        if (passes[i].id == name)
            return std::uint64_t{1} << i;
    return 0;
}

static_assert(rewrite_binary("add", true, false, 7, 0, 2)->kind == ReplacementKind::Lhs);
static_assert(rewrite_binary("add", true, false, 7, 0, 2, only("fold-wrapping-arithmetic"))->bits == 7);
static_assert(!rewrite_binary("add", true, false, 7, 0, 2, only("mul-one")));

int main()
{
    unsigned count = 0;
    if (run_lowering_stages([&](const LoweringStage &) { return ++count != 3; }) || count != 3)
        return 1;
    count = 0;
    if (!run_lowering_stages([&](const LoweringStage &) { ++count; return true; }) || count != 5)
        return 1;

    const auto literal = [](const std::string &text) -> std::optional<std::uint64_t> {
        if (text == "-")
            return {};
        return std::stoull(text);
    };
    std::string opcode, a, b;
    unsigned level;
    bool all_i64, same;
    while (std::cin >> opcode >> level >> all_i64 >> same >> a >> b) {
        const auto r = rewrite_binary(opcode, all_i64, same, literal(a), literal(b), level);
        if (!r)
            std::cout << "none\\n";
        else if (r->kind == ReplacementKind::Constant)
            std::cout << r->bits << '\\n';
        else
            std::cout << (r->kind == ReplacementKind::Lhs ? "lhs\\n" : "rhs\\n");
    }
    return !std::cin.eof();
}
"""


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cephyr-mir-catalog-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.catalog = self.root / "catalog"
        self.catalog.mkdir()
        for suffix in ("mirpass", "mirlower"):
            for source in CATALOG.glob("*." + suffix):
                shutil.copyfile(source, self.catalog / source.name)
        self.output = self.root / "mir_catalog.hpp"

    def generate(self):
        return subprocess.run(
            [sys.executable, str(CATALOG / "compile_catalog.py"),
             "--catalog", str(self.catalog), "--output", str(self.output)],
            text=True, capture_output=True,
        )

    def edit(self, filename, edit):
        path = self.catalog / filename
        data = json.loads(path.read_text())
        edit(data)
        path.write_text(json.dumps(data))

    def reject(self, pattern):
        self.output.write_text("previous output")
        generated = self.generate()
        self.assertEqual(generated.returncode, 1, generated)
        self.assertIn(pattern, generated.stderr)
        self.assertEqual(self.output.read_text(), "previous output")

    def test_catalog_is_complete_and_deterministic(self):
        passes, _ = read_passes(self.catalog)
        self.assertGreater(len(passes), 20)
        self.assertEqual(len(list(self.catalog.glob("*.mirlower"))), 5)
        self.assertEqual(self.generate().returncode, 0)
        self.assertEqual(self.output.read_text(), compile_catalog(CATALOG))

    def test_rejects_unsupported_opcode(self):
        self.edit("add-zero.mirpass", lambda d: d["rules"][0].update(opcode="add_checked_s"))
        self.reject("unsupported opcode")

    def test_rejects_unsafe_equation(self):
        self.edit("sub-zero.mirpass", lambda d: d["rules"][0].update(match="LhsZero", replace="Rhs"))
        self.reject("unsupported identity")

    def test_rejects_duplicate_rule(self):
        self.edit("add-zero.mirpass", lambda d: d["rules"].append(d["rules"][0]))
        self.reject("duplicate match")

    def test_rejects_duplicate_key(self):
        path = self.catalog / "add-zero.mirpass"
        path.write_text(path.read_text().replace('"version": 1,', '"version": 1, "version": 1,'))
        self.reject("duplicate JSON key")

    def test_rejects_disabled_level(self):
        self.edit("add-zero.mirpass", lambda d: d.update(min_opt=0))
        self.reject("min_opt")

    def test_rejects_missing_stage(self):
        (self.catalog / "project-types.mirlower").unlink()
        self.reject("expected exactly")

    def test_rejects_unsupported_stage_contract(self):
        self.edit("project-types.mirlower", lambda d: d.update(output="ordered-blocks"))
        self.reject("unsupported stage contract")

    def test_matcher(self):
        generated = self.generate()
        self.assertEqual(generated.returncode, 0, generated)
        source = self.root / "test.cpp"
        source.write_text(CPP_TEST)
        executable = self.root / "test"
        subprocess.run(shlex.split(os.environ.get("CXX", "c++")) +
                       ["-std=c++17", "-pedantic", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(executable)], check=True)
        cases = []

        def add(op, a, b, *, level, same=False, lhs=False, rhs=False, typed=True, hits=True):
            line = (f"{op} {level} {int(typed)} {int(same)} "
                    f"{a if lhs else '-'} {b if rhs else '-'}")
            expected = ORACLE[op](a, b) & MASK if hits else None
            cases.append((line, a, b, expected))

        _, descriptors = read_passes(self.catalog)
        for op, match, action, _ in descriptors:
            if match == "BothConstants":
                for a in WORDS:
                    for b in WORDS:
                        add(op, a, b, level=2, lhs=True, rhs=True)
                add(op, 7, 3, level=1, lhs=True, rhs=True, hits=False)
            elif match == "SameOperands":
                for word in WORDS:
                    add(op, word, word, level=1, same=True)
            else:
                word = LITERALS[match[3:]]
                for value in WORDS:
                    if match.startswith("Lhs"):
                        add(op, word, value, level=1, lhs=True)
                    else:
                        add(op, value, word, level=1, rhs=True)

        add("add", 42, 0, level=0, rhs=True, hits=False)
        add("add", 42, 0, level=3, rhs=True, typed=False, hits=False)
        add("add", 42, 0, level=3, hits=False)  # No dominating literal supplied.
        add("sub", 0, 42, level=3, lhs=True, hits=False)
        add("ashr", SIGN, 64, level=1, rhs=True, hits=False)
        add("eq", 0, 1, level=3, lhs=True, rhs=True, same=True, hits=False)
        for op in ("add_checked_s", "div_checked_s", "fadd", "ptr_add", "load", "call", "unknown"):
            add(op, 0, 1, level=3, lhs=True, rhs=True, hits=False)

        output = subprocess.run([str(executable)], text=True, capture_output=True, check=True,
                                input="\n".join(line for line, *_ in cases) + "\n")
        results = output.stdout.splitlines()
        self.assertEqual(len(results), len(cases))
        for (line, a, b, expected), result in zip(cases, results):
            actual = {"none": None, "lhs": a, "rhs": b}[result] if not result.isdecimal() else int(result)
            self.assertEqual(actual, expected, line)


if __name__ == "__main__":
    unittest.main()
