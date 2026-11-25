#!/usr/bin/env python3
"""Compile Cephyr's closed MIR catalogs into one self-contained C++17 header."""

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path


FORMAT = 1
IDENTIFIER = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*")

# Safe identities for the Archon MachineIR v1 i64 carrier. The catalog
# selects from these reviewed equations; it cannot declare new semantics.
IDENTITIES = {
    "add": {"RhsZero": "Lhs"},
    "sub": {"RhsZero": "Lhs", "SameOperands": "Zero"},
    "mul": {"RhsOne": "Lhs", "RhsZero": "Zero"},
    "and": {"RhsZero": "Zero", "RhsAllOnes": "Lhs", "SameOperands": "Lhs"},
    "or": {"RhsZero": "Lhs", "RhsAllOnes": "AllOnes", "SameOperands": "Lhs"},
    "xor": {"RhsZero": "Lhs", "SameOperands": "Zero"},
    "shl": {"RhsZero": "Lhs"},
    "lshr": {"RhsZero": "Lhs"},
    "ashr": {"RhsZero": "Lhs"},
    "eq": {"SameOperands": "One"},
    "slt": {"SameOperands": "Zero"},
    "ult": {"SameOperands": "Zero"},
    "div_u": {"RhsOne": "Lhs"},
    "div_s": {"RhsOne": "Lhs"},
    "rem_u": {"RhsOne": "Zero"},
    "rem_s": {"RhsOne": "Zero"},
}

for opcode in ("add", "mul", "and", "or", "xor"):
    for match, action in tuple(IDENTITIES[opcode].items()):
        if match.startswith("Rhs"):
            IDENTITIES[opcode]["Lhs" + match[3:]] = "Rhs" if action == "Lhs" else action

STAGES = (
    ("VerifyGraph", "verify-graph", "cephyr-mir", "verified-graph"),
    ("ProjectTypes", "project-types", "verified-graph", "wire-types"),
    ("LinearizeBlocks", "linearize-blocks", "wire-types", "ordered-blocks"),
    ("AssignWireIds", "assign-wire-ids", "ordered-blocks", "numbered-blocks"),
    ("EmitArchonMir", "emit-archon-mir", "numbered-blocks", "archon-mir-v1"),
)


def unique_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def fields(obj, *keys):
    if type(obj) is not dict or obj.keys() != set(keys):
        raise ValueError("expected exactly these fields: " + ", ".join(keys))


def text(value, name):
    if not isinstance(value, str) or not value or not all(" " <= c <= "~" for c in value):
        raise ValueError(f"{name} must be nonempty printable ASCII")
    return value


def read(path, *keys):
    try:
        data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_keys)
        fields(data, "version", "id", "summary", *keys)
        if type(data["version"]) is not int or data["version"] != FORMAT:
            raise ValueError(f"expected catalog format version {FORMAT}")
        if not IDENTIFIER.fullmatch(text(data["id"], "id")) or path.stem != data["id"]:
            raise ValueError("id must be lowercase kebab-case and match the file name")
        text(data["summary"], "summary")
        return data
    except ValueError as error:
        raise ValueError(f"{path}: {error}") from error


def read_passes(directory):
    passes = []
    rules = []
    seen = set()
    for path in sorted(directory.glob("*.mirpass")):
        entry = read(path, "min_opt", "rules")
        try:
            level = entry["min_opt"]
            if type(level) is not int or not 1 <= level <= 3:
                raise ValueError("min_opt must be an integer in [1, 3]")
            if type(entry["rules"]) is not list or not entry["rules"]:
                raise ValueError("rules must be a nonempty array")
            for rule in entry["rules"]:
                fields(rule, "opcode", "match", "replace")
                opcode, match, action = (text(rule[key], key) for key in ("opcode", "match", "replace"))
                if opcode not in IDENTITIES:
                    raise ValueError(f"unsupported opcode {opcode!r}")
                if match == "BothConstants" and action == "Fold":
                    if level < 2:
                        raise ValueError("constant folding requires min_opt >= 2")
                elif IDENTITIES[opcode].get(match) != action:
                    raise ValueError(f"unsupported identity: {opcode} {match} -> {action}")
                if (opcode, match) in seen:
                    raise ValueError(f"duplicate match for {opcode} {match}")
                seen.add((opcode, match))
                rules.append((opcode, match, action, len(passes)))
            passes.append(entry)
        except ValueError as error:
            raise ValueError(f"{path}: {error}") from error
    if not passes:
        raise ValueError(f"{directory}: no .mirpass files")
    return passes, sorted(rules, key=lambda r: (r[0], passes[r[3]]["min_opt"], r[1]))


def read_stages(directory):
    entries = {}
    for path in sorted(directory.glob("*.mirlower")):
        stage = read(path, "kind", "input", "output")
        entries[stage["id"]] = stage
    if entries.keys() != {row[1] for row in STAGES}:
        raise ValueError(f"{directory}: expected exactly: " + ", ".join(row[1] for row in STAGES))
    ordered = []
    for kind, stage_id, input_name, output_name in STAGES:
        stage = entries[stage_id]
        if (stage["kind"], stage["input"], stage["output"]) != (kind, input_name, output_name):
            raise ValueError(f"{directory / (stage_id + '.mirlower')}: unsupported stage contract")
        ordered.append(stage)
    return ordered


PRELUDE = """\
// Generated by compile_catalog.py; edit .mirpass/.mirlower sources instead.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cephyr::mir::catalog {

inline constexpr unsigned format_version = 1;

enum class Match : unsigned char {
    LhsZero, RhsZero, LhsOne, RhsOne, LhsAllOnes, RhsAllOnes, SameOperands, BothConstants
};
enum class Action : unsigned char { Lhs, Rhs, Zero, One, AllOnes, Fold };
enum class ReplacementKind : unsigned char { Lhs, Rhs, Constant };
enum class StageKind : unsigned char {
    VerifyGraph, ProjectTypes, LinearizeBlocks, AssignWireIds, EmitArchonMir
};

struct Pass {
    std::string_view id, summary;
    unsigned min_opt;
};
struct Rule {
    std::string_view opcode;
    Match match;
    Action action;
    std::size_t pass_index;
};
struct Replacement {
    ReplacementKind kind;
    std::uint64_t bits; // Only meaningful for Constant.
};
struct LoweringStage {
    StageKind kind;
    std::string_view id, input, output, summary;
};
"""

MATCHER = """\
namespace detail {

inline constexpr std::uint64_t all_ones = ~std::uint64_t{0};
inline constexpr std::uint64_t sign_bit = std::uint64_t{1} << 63;

constexpr bool matches(Match match, bool same, std::optional<std::uint64_t> lhs,
                       std::optional<std::uint64_t> rhs) noexcept
{
    switch (match) {
    case Match::LhsZero: return lhs && *lhs == 0;
    case Match::RhsZero: return rhs && *rhs == 0;
    case Match::LhsOne: return lhs && *lhs == 1;
    case Match::RhsOne: return rhs && *rhs == 1;
    case Match::LhsAllOnes: return lhs && *lhs == all_ones;
    case Match::RhsAllOnes: return rhs && *rhs == all_ones;
    case Match::SameOperands: return same;
    case Match::BothConstants: return lhs && rhs;
    }
    return false;
}

// All arithmetic is on unsigned bit patterns. In particular, never evaluate
// a host signed overflow, negative right shift, or signed MIN / -1.
constexpr std::optional<std::uint64_t> fold(std::string_view op, std::uint64_t a,
                                           std::uint64_t b) noexcept
{
    if (op == "add") return a + b;
    if (op == "sub") return a - b;
    if (op == "mul") return a * b;
    if (op == "and") return a & b;
    if (op == "or") return a | b;
    if (op == "xor") return a ^ b;
    if (op == "eq") return a == b;
    if (op == "slt") return (a ^ sign_bit) < (b ^ sign_bit);
    if (op == "ult") return a < b;
    if (op == "shl") return a << (b & 63);
    if (op == "lshr") return a >> (b & 63);
    if (op == "ashr") {
        const auto shift = b & 63;
        const auto high = shift && (a & sign_bit) ? all_ones << (64 - shift) : 0;
        return (a >> shift) | high;
    }
    if (op == "div_u") return b ? a / b : all_ones;
    if (op == "rem_u") return b ? a % b : a;
    if (op == "div_s" || op == "rem_s") {
        const bool remainder = op == "rem_s";
        if (!b)
            return remainder ? a : all_ones;
        const bool a_negative = (a & sign_bit) != 0;
        const bool b_negative = (b & sign_bit) != 0;
        const auto m = a_negative ? 0 - a : a;
        const auto n = b_negative ? 0 - b : b;
        const auto r = remainder ? m % n : m / n;
        return (remainder ? a_negative : a_negative != b_negative) ? 0 - r : r;
    }
    return {};
}

} // namespace detail

// Inputs must be verified (i64, i64) -> i64 SSA with dominating facts.
// Replacing the root must not move/delete producers or other graph nodes.
static_assert(passes.size() < 64, "MIR pass mask needs more bits; all-ones is reserved");
constexpr std::optional<Replacement> rewrite_binary(
    std::string_view opcode, bool all_i64, bool same_operands,
    std::optional<std::uint64_t> lhs_const, std::optional<std::uint64_t> rhs_const,
    unsigned optimization_level, std::uint64_t pass_mask = detail::all_ones) noexcept
{
    if (!optimization_level || !all_i64 || (!same_operands && !lhs_const && !rhs_const) ||
        (same_operands && lhs_const && rhs_const && *lhs_const != *rhs_const))
        return {};

    std::size_t first = 0, last = rules.size();
    while (first != last) {
        const auto mid = first + (last - first) / 2;
        if (rules[mid].opcode < opcode)
            first = mid + 1;
        else
            last = mid;
    }
    for (; first < rules.size() && rules[first].opcode == opcode; ++first) {
        const auto &rule = rules[first];
        if (optimization_level < passes[rule.pass_index].min_opt)
            break;
        if (pass_mask != detail::all_ones &&
            (pass_mask & (std::uint64_t{1} << rule.pass_index)) == 0)
            continue;
        if (!detail::matches(rule.match, same_operands, lhs_const, rhs_const))
            continue;
        switch (rule.action) {
        case Action::Lhs: return Replacement{ReplacementKind::Lhs, 0};
        case Action::Rhs: return Replacement{ReplacementKind::Rhs, 0};
        case Action::Zero: return Replacement{ReplacementKind::Constant, 0};
        case Action::One: return Replacement{ReplacementKind::Constant, 1};
        case Action::AllOnes: return Replacement{ReplacementKind::Constant, detail::all_ones};
        case Action::Fold:
            if (lhs_const && rhs_const)
                if (auto value = detail::fold(opcode, *lhs_const, *rhs_const))
                    return Replacement{ReplacementKind::Constant, *value};
            return {};
        }
    }
    return {};
}

// The caller binds each supported stage and returns false on failure.
template <class Execute>
bool run_lowering_stages(Execute &&execute)
{
    for (const auto &stage : lowering_stages)
        if (!execute(stage))
            return false;
    return true;
}

} // namespace cephyr::mir::catalog
"""


def cpp_array(type_name, name, rows):
    members = "".join("    {" + ", ".join(row) + "},\n" for row in rows)
    return f"\ninline constexpr std::array<{type_name}, {len(rows)}> {name}{{{{\n{members}}}}};\n"


def compile_catalog(directory):
    passes, rules = read_passes(directory)
    stages = read_stages(directory)
    quote = json.dumps
    return PRELUDE + cpp_array(
        "Pass", "passes",
        [(quote(p["id"]), quote(p["summary"]), str(p["min_opt"])) for p in passes],
    ) + cpp_array(
        "Rule", "rules",
        [(quote(op), f"Match::{m}", f"Action::{a}", str(p)) for op, m, a, p in rules],
    ) + cpp_array(
        "LoweringStage", "lowering_stages",
        [(f"StageKind::{s['kind']}", *(quote(s[k]) for k in ("id", "input", "output", "summary")))
         for s in stages],
    ) + "\n" + MATCHER


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    temporary_path = None
    try:
        contents = compile_catalog(args.catalog)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            dir=args.output.parent, mode="w", encoding="utf-8", newline="\n", delete=False
        ) as temporary:
            temporary_path = Path(temporary.name)
            temporary.write(contents)
        temporary_path.replace(args.output)
    except (OSError, ValueError) as error:
        print(f"compile_catalog: {error}", file=sys.stderr)
        return 1
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
