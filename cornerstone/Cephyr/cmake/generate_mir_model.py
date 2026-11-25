"""Project checked acsem JSON to Cephyr's native MIR records and descriptors."""

import argparse
import json
from pathlib import Path
import re


class SchemaError(ValueError):
    pass


def require(condition, reason):
    if not condition:
        raise SchemaError(reason)


def cpp_name(name):
    require(re.fullmatch(r"[a-z][a-z_0-9]*", name), f"invalid C++ name: {name!r}")
    # All remaining names are intentional, ordinary identifiers in MIR.acsem.
    return name + "_" if name in {"and", "or", "xor", "const", "void"} else name


def members(model, sort):
    names = model["sorts"][sort]
    for name in names:
        ctor = model["constructors"][name]
        require(ctor["sort"] == sort and not ctor["fields"],
                f"{name} must be a nullary {sort} constructor")
    return names


def literals(rule):
    op = rule["op"]
    if op == "combine" and rule["symbol"] == "and then":
        return literals(rule["left"]) + literals(rule["right"])
    if op == "give" and rule["arg"]["op"] in {"token", "integer"}:
        return [rule["arg"]["value"]]
    raise SchemaError("expected a tuple of literal 'give' actions joined by 'and then'")


RESULTS = ("none", "required", "optional")
PAYLOADS = ("none", "word", "symbol", "call", "return_value")
EFFECTS = ("pure", "ordered", "read", "write", "call", "may_trap", "terminator")


def descriptors(model):
    for name in sorted(members(model, "Opcode")):
        rule = model["rules"][name]
        if name == "invalid_opcode":
            require(rule["op"] == "fail", "the invalid opcode must fail")
            continue
        desc = literals(rule)
        require(len(desc) == 5, f"wrong descriptor length for {name}")
        result, inputs, payload, effects, successors = desc
        require(result in RESULTS and type(inputs) is int and 0 <= inputs <= 2
                and payload in PAYLOADS and effects in EFFECTS and type(successors) is int
                and 0 <= successors <= 2, f"bad descriptor for {name}")
        require((result == "optional") == (payload == "call") == (effects == "call")
                and (effects != "terminator" or result == "none")
                and (not successors or effects == "terminator")
                and (payload != "return_value" or (not inputs and effects == "terminator")),
                f"incompatible descriptor fields for {name}")
        yield name, result, inputs, payload, effects, successors


# Acsem terms use signed integers and identifier tokens. The native projection
# assigns each record field its host representation, not its interpreter limit.
NATIVE_TYPES = {
    "integer": "std::size_t",
    "identifier": "std::string",
    "Word": "std::uint64_t",
}
DEFAULTS = {
    "NodeId": "no_node",
    "BlockId": "no_block",
    "ValueId": "no_value",
    "Opcode": "Opcode::invalid",
    "ValueType": "ValueType::void_",
    "NodeKind": "NodeKind::operation",
    "EdgeKind": "EdgeKind::data",
}


def record(model, sort):
    names = model["sorts"][sort]
    require(len(names) == 1, f"{sort} must be a record sort")
    fields = model["constructors"][names[0]]["fields"]
    for field in fields:
        require(field["type"] in NATIVE_TYPES or field["type"] in DEFAULTS
                or field["type"] == "InputRange", f"unsupported field {sort}.{field['name']}")
    yield f"struct {sort} {{"
    for field in fields:
        typ = field["type"]
        name = cpp_name(field["name"])
        yield f"    {NATIVE_TYPES.get(typ, typ)} {name}{{{DEFAULTS.get(typ, '')}}};"
    yield "};"


def enumeration(typ, names, underlying="std::uint8_t"):
    return f"enum class {typ} : {underlying} {{ {', '.join(map(cpp_name, names))} }};"


def emit(model):
    require(model["name"] == "CephyrMIR" and model["version"] == 1 and model["start"] == "Probe",
            "expected checked CephyrMIR acsem version 1 JSON")
    rows = list(descriptors(model))
    require("invalid_opcode" in members(model, "Opcode"), "missing invalid opcode")
    types = ["void" if name == "void_type" else name for name in members(model, "ValueType")]
    yield """// Generated from the checked Cephyr MIR.acsem. Do not edit.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cephyr::mir {

using NodeId = std::uint32_t;
using BlockId = std::uint32_t;
using ValueId = std::uint32_t;
inline constexpr NodeId no_node = std::numeric_limits<NodeId>::max();
inline constexpr BlockId no_block = std::numeric_limits<BlockId>::max();
inline constexpr ValueId no_value = std::numeric_limits<ValueId>::max();
"""
    yield enumeration("EdgeKind", members(model, "EdgeKind"))
    node_kinds = members(model, "NodeKind")
    require(all(name.endswith("_node") for name in node_kinds), "expected *_node constructors")
    yield enumeration("NodeKind", (name.removesuffix("_node") for name in node_kinds))
    yield enumeration("ValueType", ["invalid", *types])
    yield enumeration("ResultKind", RESULTS)
    yield enumeration("PayloadKind", PAYLOADS)
    yield enumeration("EffectClass", EFFECTS)
    yield "enum class Opcode : std::uint16_t {"
    yield "    invalid,"
    for name, *_ in rows:
        yield f"    {cpp_name(name)},"
    yield "};"
    yield """
struct OpcodeInfo {
    Opcode opcode;
    std::string_view name;
    ResultKind result;
    std::uint8_t fixed_inputs;
    PayloadKind payload;
    EffectClass effects;
    std::uint8_t successors;

    constexpr bool is_terminator() const { return effects == EffectClass::terminator; }
    constexpr bool is_ordered() const { return effects != EffectClass::pure; }
    constexpr bool accepts_inputs(std::size_t count) const
    {
        if (payload == PayloadKind::call)
            return count >= fixed_inputs;
        if (payload == PayloadKind::return_value)
            return count <= 1;
        return count == fixed_inputs;
    }
};

inline constexpr std::array opcode_table{"""
    for name, result, inputs, payload, effects, successors in rows:
        yield (f'    OpcodeInfo{{Opcode::{cpp_name(name)}, "{name}", ResultKind::{result}, '
               f'{inputs}, PayloadKind::{payload}, EffectClass::{effects}, {successors}}},')
    yield """};

constexpr const OpcodeInfo *opcode_info(Opcode op)
{
    const auto index = static_cast<std::size_t>(op);
    return index > 0 && index <= opcode_table.size() ? &opcode_table[index - 1] : nullptr;
}

constexpr const OpcodeInfo *lookup_opcode(std::string_view name)
{
    const auto it = std::lower_bound(opcode_table.begin(), opcode_table.end(), name,
        [](const OpcodeInfo &info, std::string_view key) { return info.name < key; });
    return it != opcode_table.end() && it->name == name ? &*it : nullptr;
}

constexpr std::string_view value_type_name(ValueType type)
{
    switch (type) {"""
    for name in types:
        yield f'    case ValueType::{cpp_name(name)}: return "{name}";'
    yield """    default: return {};
    }
}

constexpr ValueType lookup_value_type(std::string_view name)
{"""
    for name in types:
        yield f'    if (name == "{name}") return ValueType::{cpp_name(name)};'
    yield """    return ValueType::invalid;
}
"""
    for sort in ("InputRange", "Input", "Node", "Block", "Successor"):
        yield from record(model, sort)
        yield ""
    yield """struct Graph {
    // One graph per function. A record's ID is its index in these flat tables.
    NodeId start = no_node;
    std::vector<Node> nodes;
    std::vector<Input> inputs;
    std::vector<Block> blocks;
    std::vector<Successor> successors;

    NodeId add_node(Node node)
    {
        if (nodes.size() >= no_node)
            throw std::length_error("Cephyr MIR node ID exhausted");
        const auto id = static_cast<NodeId>(nodes.size());
        nodes.push_back(std::move(node));
        return id;
    }

    // The source range must not alias this graph's input storage.
    InputRange add_inputs(std::span<const Input> edges)
    {
        const InputRange range{inputs.size(), edges.size()};
        inputs.insert(inputs.end(), edges.begin(), edges.end());
        return range;
    }

    InputRange add_inputs(std::initializer_list<Input> edges)
    {
        return add_inputs(std::span<const Input>{edges.begin(), edges.size()});
    }

    std::optional<std::span<const Input>> input_slice(InputRange range) const
    {
        if (range.first > inputs.size() || range.count > inputs.size() - range.first)
            return std::nullopt;
        return std::span<const Input>{inputs}.subspan(range.first, range.count);
    }
};

} // namespace cephyr::mir
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("schema", type=Path, help="checked JSON from acsem --emit json")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        header = "\n".join(emit(json.loads(args.schema.read_text(encoding="utf-8"))))
        args.output.write_text(header, encoding="utf-8")
    except (OSError, ValueError, TypeError, KeyError) as exc:
        parser.exit(1, f"{args.schema}: {exc}\n")


if __name__ == "__main__":
    main()
