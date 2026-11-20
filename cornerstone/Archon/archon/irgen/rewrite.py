"""Generate bounded C99 term rewriting and equality-saturation support."""

from __future__ import annotations

import hashlib
import json
import re
from collections.abc import Mapping
from urllib.parse import quote

from ..diagnostics import ArchonError


NAME = "rewrite"
ALIASES = ("rewriter", "rewriting", "egraph", "e-graph", "eqsat")
STAGE_ORDER = 100

_BANK_SCHEMA = 1
_MAX_PATTERN_ARITY = 4


def _oracle_enabled(context: object) -> bool:
    if not isinstance(context, Mapping):
        return True
    options = context.get("options", {})
    if options is None:
        options = {}
    if not isinstance(options, Mapping):
        raise ArchonError("rewrite options must be an object")
    enabled = options.get("oracle", True)
    if type(enabled) is not bool:
        raise ArchonError("rewrite option 'oracle' must be boolean")
    return enabled


def emit(context):
    """Emit the rewrite runtime and one macro pattern bank per selected ArchDB."""

    outputs: dict[str, str] = {}
    bank_names: list[str] = []
    fact_count = 0
    rule_ids = []
    for database, filename, facts, rules in generated_rule_banks(context):
        architecture = str(database.get("architecture", "architecture"))
        outputs[filename] = _pattern_bank(architecture, facts, rules)
        bank_names.append(filename)
        fact_count += len(facts)
        rule_ids.extend(rule["id"] for rule in rules)

    includes = "\n".join(f'#include "{name}"' for name in bank_names)
    oracle_enabled = _oracle_enabled(context)
    oracle_include = '#include "Oracle.h"\n' if oracle_enabled else ""
    oracle_declarations = (
        _ORACLE_DECLARATIONS if oracle_enabled else ""
    )
    oracle_implementation = (
        _ORACLE_IMPLEMENTATION if oracle_enabled else ""
    )
    outputs["Rewrite.h"] = (
        _HEADER.replace("@ORACLE_INCLUDE@", oracle_include)
        .replace("@ORACLE_DECLARATIONS@", oracle_declarations)
    )
    outputs["Rewrite.c"] = (
        _SOURCE.replace("@PATTERN_BANK_INCLUDES@", includes)
        .replace("@GENERATED_FACT_COUNT@", str(fact_count))
        .replace("@GENERATED_RULE_COUNT@", str(len(rule_ids)))
        .replace(
            "@GENERATED_RULE_IDS@",
            "\n".join(f"    {_c_string(rule_id)}," for rule_id in rule_ids)
            or "    NULL,",
        )
        .replace("@ORACLE_INCLUDE@", oracle_include)
        .replace("@ORACLE_IMPLEMENTATION@", oracle_implementation)
    )
    return outputs


def _selected_databases(context) -> list[dict]:
    """Accept the small context shapes used by the generator and unit tests."""

    if context is None:
        return []
    if isinstance(context, Mapping):
        if "architecture" in context and "instructions" in context:
            return [dict(context)]
        value = next(
            (
                context[key]
                for key in (
                    "architectures",
                    "archdbs",
                    "databases",
                    "selected_archdbs",
                    "selected_databases",
                    "targets",
                )
                if key in context
            ),
            [],
        )
    elif isinstance(context, (list, tuple)):
        value = context
    else:
        value = next(
            (
                getattr(context, key)
                for key in (
                    "architectures",
                    "archdbs",
                    "databases",
                    "selected_archdbs",
                    "selected_databases",
                    "targets",
                )
                if hasattr(context, key)
            ),
            [],
        )
        if callable(value):
            value = value()
    if value is None:
        value = []
    if isinstance(value, Mapping):
        if "architecture" in value and "instructions" in value:
            value = [value]
        else:
            value = value.values()
    result = []
    for entry in value:
        if isinstance(entry, Mapping):
            database = entry
        elif isinstance(entry, tuple) and len(entry) == 2 and isinstance(entry[1], Mapping):
            database = entry[1]
        else:
            database = next(
                (
                    getattr(entry, key)
                    for key in ("database", "archdb", "db")
                    if hasattr(entry, key)
                ),
                None,
            )
        if not isinstance(database, Mapping):
            raise ArchonError("rewrite generation requires selected ArchDB mappings")
        result.append(dict(database))
    return result


def _sanitize(name: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_]", "_", name)
    result = re.sub(r"_+", "_", result).strip("_") or "architecture"
    if result[0].isdigit():
        result = "_" + result
    return result


def _bank_stems(databases: list[dict]) -> list[str]:
    """Return deterministic, case-insensitively unique filename stems."""

    records = []
    groups: dict[str, list[int]] = {}
    for index, database in enumerate(databases):
        architecture = str(database.get("architecture", "architecture"))
        base = _sanitize(architecture)
        encoded = json.dumps(
            database,
            ensure_ascii=True,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("ascii")
        fingerprint = hashlib.sha256(encoded).hexdigest()[:12]
        records.append((base, fingerprint))
        groups.setdefault(base.casefold(), []).append(index)
    proposals = [base for base, _ in records]
    for indexes in groups.values():
        if len(indexes) <= 1:
            continue
        occurrences: dict[tuple[str, str], int] = {}
        for index in indexes:
            base, fingerprint = records[index]
            key = (base.casefold(), fingerprint)
            occurrence = occurrences.get(key, 0) + 1
            occurrences[key] = occurrence
            proposals[index] = (
                f"{base}_{fingerprint}"
                if occurrence == 1
                else f"{base}_{fingerprint}_{occurrence}"
            )
    used: set[str] = set()
    result = []
    for proposal in proposals:
        candidate = proposal
        suffix = 2
        while candidate.casefold() in used:
            candidate = f"{proposal}_{suffix}"
            suffix += 1
        used.add(candidate.casefold())
        result.append(candidate)
    return result


def _c_string(value: object) -> str:
    pieces = ['"']
    for byte in str(value).encode("utf-8"):
        if byte == 34:
            pieces.append(r"\"")
        elif byte == 92:
            pieces.append(r"\\")
        elif byte == 10:
            pieces.append(r"\n")
        elif byte == 13:
            pieces.append(r"\r")
        elif byte == 9:
            pieces.append(r"\t")
        elif 32 <= byte < 127 and byte != 63:
            pieces.append(chr(byte))
        else:
            pieces.append(f"\\{byte:03o}")
    pieces.append('"')
    return "".join(pieces)


def _hash(text: str) -> int:
    """Stable FNV-1a opcode used by generated facts and tests."""

    value = 2166136261
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value or 1


def _u32(value: int) -> str:
    return f"UINT32_C(0x{value & 0xFFFFFFFF:08x})"


def _u64(value: int) -> str:
    return f"UINT64_C(0x{value & 0xFFFFFFFFFFFFFFFF:016x})"


def _semantic_key(expr: dict) -> str:
    ty = expr.get("type") or {}
    return f"sem:{expr.get('op')}:{ty.get('kind', 'bits')}:{ty.get('width', 0)}"


def _pure_root(instruction: dict):
    """Return a directly representable pure result expression, or ``None``."""

    program = instruction.get("semantics")
    if not isinstance(program, Mapping):
        return None
    effects = program.get("effects")
    if not isinstance(effects, Mapping):
        return None
    allowed_effects = {
        "reads",
        "writes",
        "state_reads",
        "state_writes",
        "stack_reads",
        "stack_writes",
        "memory_reads",
        "memory_writes",
    }
    if any(value for key, value in effects.items() if key not in allowed_effects):
        return None
    for key in (
        "state_reads",
        "state_writes",
        "stack_reads",
        "stack_writes",
        "memory_reads",
        "memory_writes",
    ):
        if effects.get(key):
            return None
    statements = program.get("statements")
    if not isinstance(statements, list):
        return None
    locals_: dict[str, dict] = {}
    writes = []
    for statement in statements:
        if not isinstance(statement, Mapping):
            return None
        op = statement.get("op")
        if op == "let" and isinstance(statement.get("name"), str) and isinstance(
            statement.get("value"), Mapping
        ):
            locals_[statement["name"]] = dict(statement["value"])
        elif op == "write" and isinstance(statement.get("value"), Mapping):
            writes.append(statement)
        else:
            return None
    if len(writes) != 1:
        return None
    output = writes[0].get("name")
    operands = {row.get("name"): row for row in instruction.get("operands", [])}
    if (
        output not in operands
        or operands[output].get("kind") != "reg"
        or operands[output].get("direction") not in {"out", "inout"}
    ):
        return None

    def resolve(expr, active=frozenset()):
        if not isinstance(expr, Mapping):
            return None
        if expr.get("op") != "local":
            return dict(expr)
        name = expr.get("name")
        if name not in locals_ or name in active:
            return None
        return resolve(locals_[name], active | {name})

    return resolve(writes[0]["value"])


def _direct_semantic_rule(database: dict, instruction: dict):
    root = _pure_root(instruction)
    if not root or root.get("op") not in {
        "+%",
        "-%",
        "*%",
        "&",
        "|",
        "^",
        "<<",
        ">>",
        "~",
    }:
        return None
    child_keys = ("value",) if root["op"] == "~" else ("left", "right")
    sources = [
        row
        for row in instruction.get("operands", [])
        if row.get("direction") in {"in", "inout"}
    ]
    source_index = {row.get("name"): index for index, row in enumerate(sources)}
    if len(sources) > _MAX_PATTERN_ARITY:
        return None
    mapping = []
    child_types = []
    for key in child_keys:
        child = root.get(key)
        if not isinstance(child, Mapping) or child.get("op") not in {"read", "immediate"}:
            return None
        name = child.get("name")
        if name not in source_index:
            return None
        mapping.append(source_index[name])
        child_types.append(dict(child.get("type") or {}))
    if len(mapping) != len(sources) or len(set(mapping)) != len(mapping):
        return None
    architecture = str(database.get("architecture", "architecture"))
    target_key = f"target:{architecture}:{instruction.get('name', '')}"
    return {
        "root": root,
        "target_opcode": _hash(target_key),
        "semantic_opcode": _hash(_semantic_key(root)),
        "arity": len(sources),
        "map": mapping,
        "child_types": child_types,
    }


def _claim_opcode(claims: dict[int, str], opcode: int, key: str) -> None:
    previous = claims.get(opcode)
    if previous is not None and previous != key:
        raise ArchonError(
            f"rewrite opcode collision between {previous!r} and {key!r}"
        )
    claims[opcode] = key


def stable_rule_id(*parts: object) -> str:
    """Namespace generated rule IDs without relying on source order."""
    return ":".join(quote(str(part), safe="") for part in parts)


def generated_rule_banks(context):
    """Yield (ArchDB, filename, target facts, rule rows) used by Rewrite.c."""
    databases = _selected_databases(context)
    opcode_claims: dict[int, str] = {}
    for database, stem in zip(databases, _bank_stems(databases)):
        facts, rules = _pattern_facts(database, opcode_claims)
        yield database, f"PatternBank_{stem}.i", facts, rules


def _pattern_facts(database: dict, opcode_claims: dict[int, str]) -> tuple[list, list]:
    architecture = str(database.get("architecture", "architecture"))
    facts = []
    rules = []
    semantic_operations: dict[tuple, dict] = {}
    for instruction in sorted(
        database.get("instructions", []), key=lambda row: str(row.get("name", ""))
    ):
        if not isinstance(instruction, Mapping):
            continue
        name = str(instruction.get("name", ""))
        target_key = f"target:{architecture}:{name}"
        target_opcode = _hash(target_key)
        _claim_opcode(opcode_claims, target_opcode, target_key)
        semantics = instruction.get("semantics")
        pure = _pure_root(dict(instruction))
        flags = 0
        if isinstance(semantics, Mapping):
            flags |= 1
        else:
            flags |= 4  # Encoding fact only: never used to make a semantic rule.
        if pure is not None:
            flags |= 2
        source_arity = sum(
            row.get("direction") in {"in", "inout"}
            for row in instruction.get("operands", [])
            if isinstance(row, Mapping)
        )
        width = int(instruction.get("width", 0) or 0)
        facts.append(
            (
                name,
                target_opcode,
                width,
                int(instruction.get("mask", 0) or 0),
                int(instruction.get("value", 0) or 0),
                flags,
                source_arity,
                max(1, (width + 7) // 8),
            )
        )
        rule = _direct_semantic_rule(database, dict(instruction))
        if rule is None:
            continue
        label = "lower." + name
        rules.append({
            **rule,
            "kind": "rename",
            "name": label,
            "id": stable_rule_id("rewrite", architecture, label),
            "instruction": name,
            "engines": 3,
            "target_cost": max(1, (width + 7) // 8),
        })
        root = rule["root"]
        _claim_opcode(
            opcode_claims, rule["semantic_opcode"], _semantic_key(root)
        )
        key = (
            root["op"],
            (root.get("type") or {}).get("kind", "bits"),
            int((root.get("type") or {}).get("width", 0)),
            tuple(
                (
                    child.get("kind", "bits"),
                    int(child.get("width", 0)),
                )
                for child in rule["child_types"]
            ),
        )
        semantic_operations[key] = rule

    for key in sorted(semantic_operations):
        rule = semantic_operations[key]
        root = rule["root"]
        operation = root["op"]
        width = int((root.get("type") or {}).get("width", 0))
        signature = ".".join(f"{kind}{size}" for kind, size in (key[1:3], *key[3]))
        common = {
            "root": root,
            "semantic_opcode": rule["semantic_opcode"],
            "child_types": rule["child_types"],
            "arity": rule["arity"],
        }
        if operation in {"+%", "*%", "&", "|", "^"}:
            label = f"commute.{operation}.{width}"
            rules.append({
                **common,
                "kind": "commute",
                "name": label,
                "id": stable_rule_id("rewrite", architecture, label, signature),
                "engines": 2,
            })
        identities = []
        if operation in {"+%", "|", "^"}:
            identities = [(0, 0), (1, 0)]
        elif operation == "-%":
            identities = [(1, 0)]
        elif operation == "*%":
            identities = [(0, 1), (1, 1)]
        elif operation == "&" and 0 < width <= 64:
            identities = [(0, (1 << width) - 1), (1, (1 << width) - 1)]
        elif operation in {"<<", ">>"}:
            identities = [(1, 0)]
        for side, value in identities:
            if side >= len(rule["child_types"]):
                continue
            child_type = rule["child_types"][side]
            child_width = int(child_type.get("width", 0))
            if not 0 < child_width <= 64:
                continue
            constant_opcode = _hash(
                f"sem:constant:{child_type.get('kind', 'bits')}:{child_width}"
            )
            constant_key = (
                f"sem:constant:"
                f"{child_type.get('kind', 'bits')}:{child_width}"
            )
            _claim_opcode(opcode_claims, constant_opcode, constant_key)
            label = f"identity.{operation}.{width}.{side}"
            rules.append({
                **common,
                "kind": "identity",
                "name": label,
                "id": stable_rule_id("rewrite", architecture, label, signature),
                "engines": 3,
                "side": side,
                "value": value,
                "constant_opcode": constant_opcode,
            })
    return facts, rules


def _pattern_bank(architecture: str, facts: list, rules: list) -> str:
    rule_lines = []
    for rule in rules:
        kind = rule["kind"]
        name = _c_string(rule["name"])
        opcode = _u32(rule["semantic_opcode"])
        engines = rule["engines"]
        if kind == "rename":
            mapping = list(rule["map"]) + [0] * (_MAX_PATTERN_ARITY - len(rule["map"]))
            rule_lines.append(
                f"ARCHON_RENAME_RULE({name}, {_u32(rule['target_opcode'])}, "
                f"{opcode}, {rule['arity']}u, "
                + ", ".join(f"{index}u" for index in mapping)
                + f", {engines}u, {rule['target_cost']}u, 1u)"
            )
        elif kind == "commute":
            rule_lines.append(f"ARCHON_COMMUTATIVE_RULE({name}, {opcode}, {engines}u)")
        else:
            assert kind == "identity"
            rule_lines.append(
                f"ARCHON_IDENTITY_RULE({name}, {opcode}, {_u32(rule['constant_opcode'])}, "
                f"{_u64(rule['value'])}, {rule['side']}u, {engines}u)"
            )

    lines = [
        "/* Generated by Archon. This file is a declarative macro include. */",
        "#if !defined(ARCHON_PATTERN_BANK_BEGIN) \\",
        " || !defined(ARCHON_TARGET_FACT) \\",
        " || !defined(ARCHON_RENAME_RULE) \\",
        " || !defined(ARCHON_IDENTITY_RULE) \\",
        " || !defined(ARCHON_COMMUTATIVE_RULE) \\",
        " || !defined(ARCHON_PATTERN_BANK_END)",
        '#error "define the complete Archon pattern-bank macro schema before inclusion"',
        "#endif",
        f"ARCHON_PATTERN_BANK_BEGIN({_BANK_SCHEMA}u, {_c_string(architecture)}, "
        f"{len(facts)}u, {len(rule_lines)}u)",
    ]
    for name, opcode, width, mask, value, flags, arity, cost in facts:
        lines.append(
            "ARCHON_TARGET_FACT("
            f"{_c_string(name)}, {_u32(opcode)}, {width}u, "
            f"{_c_string(hex(mask))}, {_c_string(hex(value))}, "
            f"{flags}u, {arity}u, {cost}u)"
        )
    lines.extend(rule_lines)
    lines.append("ARCHON_PATTERN_BANK_END()")
    return "\n".join(lines) + "\n"


_HEADER = r"""/* Generated by Archon: bounded graph rewriting and e-graphs. */
#ifndef ARCHON_GENERATED_REWRITE_H
#define ARCHON_GENERATED_REWRITE_H

#include "Graphs.h"
@ORACLE_INCLUDE@

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHON_REWRITE_PATTERN_ARITY 4u
#define ARCHON_REWRITE_MAX_ARITY 8u
#define ARCHON_REWRITE_NODE_NONE UINT32_MAX

typedef ArchonGraphNodeId ArchonGraphsNodeId;

typedef enum ArchonRewriteStatus {
    ARCHON_REWRITE_OK = 0,
    ARCHON_REWRITE_INVALID_ARGUMENT,
    ARCHON_REWRITE_OUT_OF_MEMORY,
    ARCHON_REWRITE_RESOURCE_LIMIT,
    ARCHON_REWRITE_GRAPH_ERROR,
    ARCHON_REWRITE_CYCLIC_TERM,
    ARCHON_REWRITE_NO_EXTRACTION,
    ARCHON_REWRITE_MALFORMED_PROGRAM,
    ARCHON_REWRITE_PROGRAM_UNAVAILABLE
} ArchonRewriteStatus;

typedef enum ArchonRewriteEngine {
    ARCHON_REWRITE_DIRECTED = 1,
    ARCHON_REWRITE_EQUALITY_SATURATION = 2
} ArchonRewriteEngine;

typedef enum ArchonRewriteRuleKind {
    ARCHON_REWRITE_RULE_RENAME = 1,
    ARCHON_REWRITE_RULE_IDENTITY = 2,
    ARCHON_REWRITE_RULE_COMMUTATIVE = 3
} ArchonRewriteRuleKind;

typedef enum ArchonGraphsFlags {
    ARCHON_GRAPHS_FLAG_NONE = 0u,
    ARCHON_GRAPHS_FLAG_CONSTANT = 1u << 0,
    ARCHON_GRAPHS_FLAG_PURE = 1u << 1,
    ARCHON_GRAPHS_FLAG_PINNED = 1u << 2,
    ARCHON_GRAPHS_FLAG_MAY_TRAP = 1u << 3,
    ARCHON_GRAPHS_FLAG_TERMINATOR = 1u << 4
} ArchonGraphsFlags;

typedef enum ArchonTargetFactFlags {
    ARCHON_TARGET_HAS_SEMANTICS = 1u << 0,
    ARCHON_TARGET_PURE_SEMANTICS = 1u << 1,
    ARCHON_TARGET_ENCODING_ONLY = 1u << 2
} ArchonTargetFactFlags;

/*
 * Rewriting uses this Graphs API rather than owning or copying a graph
 * implementation.  read() may return a borrowed child array valid until the
 * next callback.  intern() must hash-cons an immutable node and return its
 * stable ID.  A nonzero callback result is reported as GRAPH_ERROR.
 */
typedef struct ArchonGraphsNodeView {
    uint32_t opcode;
    uint32_t flags;
    uint64_t payload;
    const ArchonGraphsNodeId *children;
    size_t child_count;
} ArchonGraphsNodeView;

typedef struct ArchonGraphsAPI {
    void *context;
    int (*read)(
        void *context, ArchonGraphsNodeId node, ArchonGraphsNodeView *view);
    int (*intern)(
        void *context,
        uint32_t opcode,
        uint64_t payload,
        uint32_t flags,
        const ArchonGraphsNodeId *children,
        size_t child_count,
        ArchonGraphsNodeId *result);
    size_t (*node_count)(void *context);
    uint64_t (*cost)(
        void *context, uint32_t opcode, uint64_t payload, uint32_t flags);
} ArchonGraphsAPI;

/*
 * This convenience term graph stores only rewrite-specific node metadata.
 * Ownership, stable IDs, and ordered data edges are provided by Graphs.c.
 * Callers with an existing hash-consed graph may instead supply callbacks
 * directly through ArchonGraphsAPI.
 */
typedef struct ArchonRewriteTermNode {
    uint32_t opcode;
    uint32_t flags;
    uint64_t payload;
    size_t child_count;
} ArchonRewriteTermNode;

typedef struct ArchonRewriteGraph {
    ArchonDFG dfg;
    ArchonRewriteTermNode *terms;
    size_t term_count;
    size_t term_capacity;
    ArchonGraphsNodeId scratch[ARCHON_REWRITE_MAX_ARITY];
    int failed;
} ArchonRewriteGraph;

ArchonRewriteStatus archon_rewrite_graph_init(ArchonRewriteGraph *graph);
void archon_rewrite_graph_dispose(ArchonRewriteGraph *graph);
ArchonRewriteStatus archon_rewrite_graph_intern(
    ArchonRewriteGraph *graph,
    uint32_t opcode,
    uint64_t payload,
    uint32_t flags,
    const ArchonGraphsNodeId *children,
    size_t child_count,
    ArchonGraphsNodeId *result);
ArchonRewriteStatus archon_rewrite_graph_read(
    ArchonRewriteGraph *graph,
    ArchonGraphsNodeId node,
    ArchonGraphsNodeView *view);
ArchonGraphsAPI archon_rewrite_graph_api(ArchonRewriteGraph *graph);

typedef struct ArchonRewriteLimits {
    size_t max_iterations;
    size_t max_applications;
    size_t max_enodes;
    size_t max_eclasses;
    size_t max_worklist;
    size_t max_arity;
} ArchonRewriteLimits;

@ORACLE_DECLARATIONS@

typedef struct ArchonRewriteStats {
    size_t iterations;
    size_t applications;
    size_t enodes;
    size_t eclasses;
    int saturated;
} ArchonRewriteStats;

typedef struct ArchonRewriteRule {
    const char *name;
    uint32_t kind;
    uint32_t lhs_opcode;
    uint32_t rhs_opcode;
    uint32_t constant_opcode;
    uint64_t constant_value;
    uint32_t arity;
    uint8_t map[ARCHON_REWRITE_PATTERN_ARITY];
    uint8_t constant_side;
    uint8_t engines;
    uint16_t reserved;
    uint32_t lhs_cost;
    uint32_t rhs_cost;
} ArchonRewriteRule;

typedef struct ArchonTargetFact {
    const char *name;
    uint32_t opcode;
    uint32_t encoding_width;
    const char *encoding_mask;
    const char *encoding_value;
    uint32_t flags;
    uint32_t arity;
    uint32_t cost;
} ArchonTargetFact;

extern const ArchonRewriteRule archon_generated_rewrite_rules[];
extern const size_t archon_generated_rewrite_rule_count;
extern const ArchonTargetFact archon_generated_target_facts[];
extern const size_t archon_generated_target_fact_count;

/* Stable Proof.py inventory ID for this generated array index, or NULL. */
const char *archon_rewrite_generated_rule_id(size_t index);

ArchonRewriteLimits archon_rewrite_default_limits(void);
const char *archon_rewrite_status_string(ArchonRewriteStatus status);
uint32_t archon_rewrite_opcode(const char *stable_name);

ArchonRewriteStatus archon_rewrite_run(
    const ArchonGraphsAPI *graphs,
    ArchonGraphsNodeId root,
    ArchonRewriteEngine engine,
    const ArchonRewriteRule *rules,
    size_t rule_count,
    const ArchonRewriteLimits *limits,
    ArchonGraphsNodeId *result,
    ArchonRewriteStats *stats);

ArchonRewriteStatus archon_rewrite_run_generated(
    const ArchonGraphsAPI *graphs,
    ArchonGraphsNodeId root,
    ArchonRewriteEngine engine,
    const ArchonRewriteLimits *limits,
    ArchonGraphsNodeId *result,
    ArchonRewriteStats *stats);

/*
 * The callback runs once per counted application in this invocation. Directed
 * events indicate replacements; e-graph events indicate additions or unions,
 * not necessarily rewrites selected by extraction. Events can precede an
 * error and are observations, not proof certificates. The callback must not
 * change the graph or re-enter this invocation.
 */
typedef void (*ArchonRewriteApplicationFn)(
    void *user, const char *stable_rule_id, ArchonRewriteEngine engine);

ArchonRewriteStatus archon_rewrite_run_generated_observed(
    const ArchonGraphsAPI *graphs,
    ArchonGraphsNodeId root,
    ArchonRewriteEngine engine,
    const ArchonRewriteLimits *limits,
    ArchonGraphsNodeId *result,
    ArchonRewriteStats *stats,
    ArchonRewriteApplicationFn observe,
    void *user);

/*
 * Defining ARCHON_REWRITE_PROGRAM_FILE to a quoted include path while
 * compiling Rewrite.c makes this function decode the Sea-of-Nodes Program.i
 * X-macros through the Graphs callbacks and run the generated pattern bank.
 * ARCHON_REWRITE_INCLUDE_PROGRAM retains the default "Program.i".  The term
 * rewriter deliberately rejects regions and control/effect edges rather than
 * dropping their semantics.
 */
ArchonRewriteStatus archon_rewrite_program(
    const ArchonGraphsAPI *graphs,
    ArchonRewriteEngine engine,
    const ArchonRewriteLimits *limits,
    ArchonGraphsNodeId *result,
    ArchonRewriteStats *stats);

#ifdef __cplusplus
}
#endif

#endif
"""


_ORACLE_DECLARATIONS = r"""
/*
 * Advisory rewrite sidecars use only these validated keys:
 *   rewrite.enabled                         BOOL
 *   rewrite.engine                          STRING ("directed" or "egraph")
 *   rewrite.max_iterations                  U64 (1..max_iterations)
 *   rewrite.operation.<decimal-id>.forbid   BOOL
 *   rewrite.operation.<decimal-id>.priority I64
 *
 * Operation IDs are canonical artifact IDs, not names.  Unknown keys remain
 * in the sidecar and are ignored.  Advice never supplies an equivalence proof.
 */
typedef struct ArchonRewriteOracleConfig {
    ArchonOracleAnnotations annotations;
    ArchonRewriteEngine engine;
    size_t max_iterations;
    int loaded;
    int enabled;
} ArchonRewriteOracleConfig;

void archon_rewrite_oracle_config_init(
    ArchonRewriteOracleConfig *config);
void archon_rewrite_oracle_config_dispose(
    ArchonRewriteOracleConfig *config);
ArchonOracleStatus archon_rewrite_oracle_config_load(
    const char *sidecar_path,
    ArchonRewriteEngine requested_engine,
    size_t max_iterations,
    ArchonRewriteOracleConfig *config,
    ArchonOracleDiagnostic *diagnostic);
ArchonOracleStatus archon_rewrite_oracle_operation(
    const ArchonRewriteOracleConfig *config,
    uint64_t operation_id,
    int *forbidden,
    int64_t *priority,
    ArchonOracleDiagnostic *diagnostic);
"""


_ORACLE_IMPLEMENTATION = r"""
static ArchonOracleStatus archon_rewrite_oracle_error(
        ArchonOracleDiagnostic *diagnostic,
        ArchonOracleStatus status,
        const char *message,
        const char *key) {
    archon_oracle_diagnostic_init(diagnostic);
    if (diagnostic != NULL) {
        diagnostic->status = status;
        if (key != NULL)
            (void)snprintf(
                diagnostic->message, sizeof diagnostic->message,
                "%s: %s", message, key);
        else
            (void)snprintf(
                diagnostic->message, sizeof diagnostic->message,
                "%s", message);
        diagnostic->message[sizeof diagnostic->message - 1u] = '\0';
    }
    return status;
}

void archon_rewrite_oracle_config_init(
        ArchonRewriteOracleConfig *config) {
    if (config == NULL)
        return;
    memset(config, 0, sizeof *config);
    archon_oracle_annotations_init(&config->annotations);
    config->engine = ARCHON_REWRITE_DIRECTED;
    config->enabled = 1;
}

void archon_rewrite_oracle_config_dispose(
        ArchonRewriteOracleConfig *config) {
    if (config == NULL)
        return;
    archon_oracle_annotations_dispose(&config->annotations);
    memset(config, 0, sizeof *config);
}

ArchonOracleStatus archon_rewrite_oracle_config_load(
        const char *sidecar_path,
        ArchonRewriteEngine requested_engine,
        size_t max_iterations,
        ArchonRewriteOracleConfig *config,
        ArchonOracleDiagnostic *diagnostic) {
    ArchonOracleStatus status;
    int enabled;
    uint64_t iterations;
    const char *engine;
    if (sidecar_path == NULL || config == NULL ||
            max_iterations == 0u ||
            (requested_engine != ARCHON_REWRITE_DIRECTED &&
             requested_engine != ARCHON_REWRITE_EQUALITY_SATURATION))
        return archon_rewrite_oracle_error(
            diagnostic, ARCHON_ORACLE_STATUS_INVALID_ARGUMENT,
            "invalid rewrite annotation arguments", NULL);
    archon_rewrite_oracle_config_init(config);
    config->engine = requested_engine;
    config->max_iterations = max_iterations;
    archon_oracle_diagnostic_init(diagnostic);
    status = archon_oracle_annotations_load(
        sidecar_path, &config->annotations, diagnostic);
    if (status == ARCHON_ORACLE_STATUS_NOT_FOUND) {
        archon_oracle_diagnostic_init(diagnostic);
        return ARCHON_ORACLE_STATUS_OK;
    }
    if (status != ARCHON_ORACLE_STATUS_OK)
        goto failed;
    config->loaded = 1;
    if (config->annotations.stage != ARCHON_ORACLE_STAGE_REWRITE) {
        status = archon_rewrite_oracle_error(
            diagnostic, ARCHON_ORACLE_STATUS_PROTOCOL_ERROR,
            "rewrite sidecar has the wrong stage", NULL);
        goto failed;
    }
    status = archon_oracle_annotations_lookup_bool(
        &config->annotations, "rewrite.enabled", &enabled);
    if (status == ARCHON_ORACLE_STATUS_OK) {
        config->enabled = enabled;
    } else if (status != ARCHON_ORACLE_STATUS_NOT_FOUND) {
        status = archon_rewrite_oracle_error(
            diagnostic, status,
            "invalid rewrite annotation", "rewrite.enabled");
        goto failed;
    }
    status = archon_oracle_annotations_lookup_string(
        &config->annotations, "rewrite.engine", &engine);
    if (status == ARCHON_ORACLE_STATUS_OK) {
        if (strcmp(engine, "directed") == 0)
            config->engine = ARCHON_REWRITE_DIRECTED;
        else if (strcmp(engine, "egraph") == 0)
            config->engine = ARCHON_REWRITE_EQUALITY_SATURATION;
        else {
            status = archon_rewrite_oracle_error(
                diagnostic, ARCHON_ORACLE_STATUS_INVALID_ARGUMENT,
                "invalid rewrite.engine value", engine);
            goto failed;
        }
    } else if (status != ARCHON_ORACLE_STATUS_NOT_FOUND) {
        status = archon_rewrite_oracle_error(
            diagnostic, status,
            "invalid rewrite annotation", "rewrite.engine");
        goto failed;
    }
    status = archon_oracle_annotations_lookup_u64(
        &config->annotations, "rewrite.max_iterations", &iterations);
    if (status == ARCHON_ORACLE_STATUS_OK) {
        if (iterations == 0u || iterations > (uint64_t)max_iterations) {
            status = archon_rewrite_oracle_error(
                diagnostic, ARCHON_ORACLE_STATUS_LIMIT_EXCEEDED,
                "rewrite.max_iterations exceeds its validated bound", NULL);
            goto failed;
        }
        config->max_iterations = (size_t)iterations;
    } else if (status != ARCHON_ORACLE_STATUS_NOT_FOUND) {
        status = archon_rewrite_oracle_error(
            diagnostic, status,
            "invalid rewrite annotation", "rewrite.max_iterations");
        goto failed;
    }
    archon_oracle_diagnostic_init(diagnostic);
    return ARCHON_ORACLE_STATUS_OK;

failed:
    archon_rewrite_oracle_config_dispose(config);
    return status;
}

ArchonOracleStatus archon_rewrite_oracle_operation(
        const ArchonRewriteOracleConfig *config,
        uint64_t operation_id,
        int *forbidden,
        int64_t *priority,
        ArchonOracleDiagnostic *diagnostic) {
    char key[96];
    int length;
    ArchonOracleStatus status;
    if (config == NULL || forbidden == NULL || priority == NULL ||
            operation_id == 0u)
        return archon_rewrite_oracle_error(
            diagnostic, ARCHON_ORACLE_STATUS_INVALID_ARGUMENT,
            "invalid rewrite operation annotation arguments", NULL);
    *forbidden = 0;
    *priority = INT64_C(0);
    archon_oracle_diagnostic_init(diagnostic);
    if (!config->loaded)
        return ARCHON_ORACLE_STATUS_OK;
    length = snprintf(
        key, sizeof key, "rewrite.operation.%" PRIu64 ".forbid",
        operation_id);
    if (length < 0 || (size_t)length >= sizeof key)
        return archon_rewrite_oracle_error(
            diagnostic, ARCHON_ORACLE_STATUS_LIMIT_EXCEEDED,
            "rewrite operation annotation key is too long", NULL);
    status = archon_oracle_annotations_lookup_bool(
        &config->annotations, key, forbidden);
    if (status != ARCHON_ORACLE_STATUS_OK &&
            status != ARCHON_ORACLE_STATUS_NOT_FOUND)
        return archon_rewrite_oracle_error(
            diagnostic, status, "invalid rewrite annotation", key);
    length = snprintf(
        key, sizeof key, "rewrite.operation.%" PRIu64 ".priority",
        operation_id);
    if (length < 0 || (size_t)length >= sizeof key)
        return archon_rewrite_oracle_error(
            diagnostic, ARCHON_ORACLE_STATUS_LIMIT_EXCEEDED,
            "rewrite operation annotation key is too long", NULL);
    status = archon_oracle_annotations_lookup_i64(
        &config->annotations, key, priority);
    if (status != ARCHON_ORACLE_STATUS_OK &&
            status != ARCHON_ORACLE_STATUS_NOT_FOUND)
        return archon_rewrite_oracle_error(
            diagnostic, status, "invalid rewrite annotation", key);
    archon_oracle_diagnostic_init(diagnostic);
    return ARCHON_ORACLE_STATUS_OK;
}
"""


_SOURCE = r"""/* Generated by Archon: bounded graph rewriting and e-graphs. */
#include "Rewrite.h"
@ORACLE_INCLUDE@

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHON_ARRAY_COUNT(A) (sizeof(A) / sizeof((A)[0]))
#define ARCHON_COST_INFINITY UINT64_MAX

@ORACLE_IMPLEMENTATION@

const ArchonTargetFact archon_generated_target_facts[] = {
#if @GENERATED_FACT_COUNT@ > 0
#define ARCHON_PATTERN_BANK_BEGIN(SCHEMA, ARCH, FACTS, RULES)
#define ARCHON_TARGET_FACT(NAME, OPCODE, WIDTH, MASK, VALUE, FLAGS, ARITY, COST) \
    {NAME, OPCODE, WIDTH, MASK, VALUE, FLAGS, ARITY, COST},
#define ARCHON_RENAME_RULE(NAME, LHS, RHS, ARITY, M0, M1, M2, M3, ENGINES, LC, RC)
#define ARCHON_IDENTITY_RULE(NAME, OP, CONST_OP, VALUE, SIDE, ENGINES)
#define ARCHON_COMMUTATIVE_RULE(NAME, OP, ENGINES)
#define ARCHON_PATTERN_BANK_END()
@PATTERN_BANK_INCLUDES@
#undef ARCHON_PATTERN_BANK_END
#undef ARCHON_COMMUTATIVE_RULE
#undef ARCHON_IDENTITY_RULE
#undef ARCHON_RENAME_RULE
#undef ARCHON_TARGET_FACT
#undef ARCHON_PATTERN_BANK_BEGIN
#else
    {NULL, 0u, 0u, NULL, NULL, 0u, 0u, 0u}
#endif
};

const size_t archon_generated_target_fact_count =
    (size_t)@GENERATED_FACT_COUNT@;

const ArchonRewriteRule archon_generated_rewrite_rules[] = {
#if @GENERATED_RULE_COUNT@ > 0
#define ARCHON_PATTERN_BANK_BEGIN(SCHEMA, ARCH, FACTS, RULES)
#define ARCHON_TARGET_FACT(NAME, OPCODE, WIDTH, MASK, VALUE, FLAGS, ARITY, COST)
#define ARCHON_RENAME_RULE(NAME, LHS, RHS, ARITY, M0, M1, M2, M3, ENGINES, LC, RC) \
    {NAME, ARCHON_REWRITE_RULE_RENAME, LHS, RHS, 0u, UINT64_C(0), ARITY, \
        {M0, M1, M2, M3}, 0u, ENGINES, 0u, LC, RC},
#define ARCHON_IDENTITY_RULE(NAME, OP, CONST_OP, VALUE, SIDE, ENGINES) \
    {NAME, ARCHON_REWRITE_RULE_IDENTITY, OP, 0u, CONST_OP, VALUE, 2u, \
        {0u, 1u, 0u, 0u}, SIDE, ENGINES, 0u, 1u, 0u},
#define ARCHON_COMMUTATIVE_RULE(NAME, OP, ENGINES) \
    {NAME, ARCHON_REWRITE_RULE_COMMUTATIVE, OP, OP, 0u, UINT64_C(0), 2u, \
        {1u, 0u, 0u, 0u}, 0u, ENGINES, 0u, 1u, 1u},
#define ARCHON_PATTERN_BANK_END()
@PATTERN_BANK_INCLUDES@
#undef ARCHON_PATTERN_BANK_END
#undef ARCHON_COMMUTATIVE_RULE
#undef ARCHON_IDENTITY_RULE
#undef ARCHON_RENAME_RULE
#undef ARCHON_TARGET_FACT
#undef ARCHON_PATTERN_BANK_BEGIN
#else
    {NULL, 0u, 0u, 0u, 0u, UINT64_C(0), 0u,
        {0u, 0u, 0u, 0u}, 0u, 0u, 0u, 0u, 0u}
#endif
};

const size_t archon_generated_rewrite_rule_count =
    (size_t)@GENERATED_RULE_COUNT@;

static const char *const archon_generated_rewrite_rule_ids[] = {
@GENERATED_RULE_IDS@
};

const char *archon_rewrite_generated_rule_id(size_t index) {
    return index < archon_generated_rewrite_rule_count
        ? archon_generated_rewrite_rule_ids[index] : NULL;
}

typedef struct ArchonRewriteObserver {
    ArchonRewriteApplicationFn call;
    void *user;
} ArchonRewriteObserver;

static void archon_rewrite_observe(
        const ArchonRewriteObserver *observer,
        size_t rule, ArchonRewriteEngine engine) {
    if (observer && observer->call)
        observer->call(observer->user, archon_rewrite_generated_rule_id(rule),
                       engine);
}

typedef struct ArchonDirectedMemo {
    ArchonGraphsNodeId input;
    ArchonGraphsNodeId output;
    uint8_t state;
} ArchonDirectedMemo;

typedef struct ArchonWorkFrame {
    ArchonGraphsNodeId node;
    size_t next_child;
    uint8_t expanded;
} ArchonWorkFrame;

typedef struct ArchonImportMemo {
    ArchonGraphsNodeId input;
    uint32_t eclass;
    uint8_t state;
} ArchonImportMemo;

typedef struct ArchonENode {
    uint32_t opcode;
    uint32_t flags;
    uint64_t payload;
    uint32_t eclass;
    uint32_t children[ARCHON_REWRITE_MAX_ARITY];
    uint8_t arity;
} ArchonENode;

typedef struct ArchonEClass {
    uint32_t parent;
} ArchonEClass;

typedef struct ArchonEGraph {
    ArchonENode *nodes;
    ArchonEClass *classes;
    uint32_t *hash_table;
    size_t node_count;
    size_t class_count;
    size_t node_capacity;
    size_t class_capacity;
    size_t hash_capacity;
} ArchonEGraph;

static int archon_size_multiply_overflows(size_t a, size_t b) {
    return a != 0u && b > SIZE_MAX / a;
}

ArchonRewriteLimits archon_rewrite_default_limits(void) {
    ArchonRewriteLimits limits;
    limits.max_iterations = 16u;
    limits.max_applications = 100000u;
    limits.max_enodes = 65536u;
    limits.max_eclasses = 65536u;
    limits.max_worklist = 65536u;
    limits.max_arity = ARCHON_REWRITE_MAX_ARITY;
    return limits;
}

const char *archon_rewrite_status_string(ArchonRewriteStatus status) {
    switch (status) {
    case ARCHON_REWRITE_OK: return "ok";
    case ARCHON_REWRITE_INVALID_ARGUMENT: return "invalid argument";
    case ARCHON_REWRITE_OUT_OF_MEMORY: return "out of memory";
    case ARCHON_REWRITE_RESOURCE_LIMIT: return "resource limit";
    case ARCHON_REWRITE_GRAPH_ERROR: return "Graphs API error";
    case ARCHON_REWRITE_CYCLIC_TERM: return "cyclic term";
    case ARCHON_REWRITE_NO_EXTRACTION: return "no finite-cost extraction";
    case ARCHON_REWRITE_MALFORMED_PROGRAM: return "malformed Program.i";
    case ARCHON_REWRITE_PROGRAM_UNAVAILABLE: return "Program.i unavailable";
    default: return "unknown rewrite status";
    }
}

uint32_t archon_rewrite_opcode(const char *stable_name) {
    uint32_t value = UINT32_C(2166136261);
    const unsigned char *cursor = (const unsigned char *)stable_name;
    if (!stable_name)
        return 0u;
    while (*cursor) {
        value ^= (uint32_t)*cursor++;
        value *= UINT32_C(16777619);
    }
    return value ? value : 1u;
}

static ArchonRewriteStatus archon_validate_limits(
        const ArchonRewriteLimits *limits) {
    if (!limits || !limits->max_iterations || !limits->max_applications
            || !limits->max_enodes || !limits->max_eclasses
            || !limits->max_worklist || !limits->max_arity
            || limits->max_arity > ARCHON_REWRITE_MAX_ARITY
            || limits->max_enodes > (size_t)UINT32_MAX
            || limits->max_eclasses > (size_t)UINT32_MAX)
        return ARCHON_REWRITE_INVALID_ARGUMENT;
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_validate_rules(
        const ArchonRewriteRule *rules, size_t count) {
    size_t i, j;
    if (count && !rules)
        return ARCHON_REWRITE_INVALID_ARGUMENT;
    for (i = 0u; i < count; ++i) {
        const ArchonRewriteRule *rule = &rules[i];
        if (!rule->name || !rule->engines
                || (rule->engines
                    & ~(ARCHON_REWRITE_DIRECTED
                        | ARCHON_REWRITE_EQUALITY_SATURATION)))
            return ARCHON_REWRITE_INVALID_ARGUMENT;
        if (rule->kind == ARCHON_REWRITE_RULE_RENAME) {
            if (!rule->lhs_opcode || !rule->rhs_opcode
                    || rule->arity > ARCHON_REWRITE_PATTERN_ARITY)
                return ARCHON_REWRITE_INVALID_ARGUMENT;
            for (j = 0u; j < rule->arity; ++j)
                if (rule->map[j] >= rule->arity)
                    return ARCHON_REWRITE_INVALID_ARGUMENT;
        } else if (rule->kind == ARCHON_REWRITE_RULE_IDENTITY) {
            if (!rule->lhs_opcode || !rule->constant_opcode
                    || rule->arity != 2u || rule->constant_side > 1u)
                return ARCHON_REWRITE_INVALID_ARGUMENT;
        } else if (rule->kind == ARCHON_REWRITE_RULE_COMMUTATIVE) {
            if (!rule->lhs_opcode || rule->arity != 2u
                    || (rule->engines & ARCHON_REWRITE_DIRECTED))
                return ARCHON_REWRITE_INVALID_ARGUMENT;
        } else {
            return ARCHON_REWRITE_INVALID_ARGUMENT;
        }
    }
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_read(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId node,
        ArchonGraphsNodeView *view,
        const ArchonRewriteLimits *limits) {
    if (node == ARCHON_REWRITE_NODE_NONE
            || graphs->read(graphs->context, node, view) != 0)
        return ARCHON_REWRITE_GRAPH_ERROR;
    if (view->child_count > limits->max_arity
            || view->child_count > ARCHON_REWRITE_MAX_ARITY
            || (view->child_count && !view->children))
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    return ARCHON_REWRITE_OK;
}

static size_t archon_directed_find(
        const ArchonDirectedMemo *memos,
        size_t memo_count,
        ArchonGraphsNodeId node) {
    size_t i;
    for (i = 0u; i < memo_count; ++i)
        if (memos[i].input == node)
            return i;
    return SIZE_MAX;
}

static size_t archon_import_find(
        const ArchonImportMemo *memos,
        size_t memo_count,
        ArchonGraphsNodeId node) {
    size_t i;
    for (i = 0u; i < memo_count; ++i)
        if (memos[i].input == node)
            return i;
    return SIZE_MAX;
}

static ArchonRewriteStatus archon_grow_array(
        void **memory,
        size_t *capacity,
        size_t needed,
        size_t limit,
        size_t element_size) {
    void *next_memory;
    size_t next;
    if (needed <= *capacity)
        return ARCHON_REWRITE_OK;
    if (needed > limit)
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    next = *capacity ? *capacity : 16u;
    if (next > limit)
        next = limit;
    if (next < needed)
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    while (next < needed) {
        size_t doubled = next <= SIZE_MAX / 2u ? next * 2u : needed;
        if (doubled < next || doubled > limit)
            doubled = limit;
        if (doubled < needed)
            return ARCHON_REWRITE_RESOURCE_LIMIT;
        next = doubled;
    }
    if (archon_size_multiply_overflows(next, element_size))
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    next_memory = realloc(*memory, next * element_size);
    if (!next_memory)
        return ARCHON_REWRITE_OUT_OF_MEMORY;
    *memory = next_memory;
    *capacity = next;
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_graph_status(
        ArchonGraphStatus status) {
    if (status == ARCHON_GRAPH_OK)
        return ARCHON_REWRITE_OK;
    if (status == ARCHON_GRAPH_OUT_OF_MEMORY)
        return ARCHON_REWRITE_OUT_OF_MEMORY;
    if (status == ARCHON_GRAPH_INVALID_ARGUMENT)
        return ARCHON_REWRITE_INVALID_ARGUMENT;
    return ARCHON_REWRITE_GRAPH_ERROR;
}

ArchonRewriteStatus archon_rewrite_graph_init(
        ArchonRewriteGraph *graph) {
    ArchonGraphStatus status;
    if (!graph)
        return ARCHON_REWRITE_INVALID_ARGUMENT;
    memset(graph, 0, sizeof(*graph));
    status = archon_dfg_init(&graph->dfg);
    return archon_graph_status(status);
}

void archon_rewrite_graph_dispose(ArchonRewriteGraph *graph) {
    if (!graph)
        return;
    archon_graph_dispose(&graph->dfg);
    free(graph->terms);
    memset(graph, 0, sizeof(*graph));
}

ArchonRewriteStatus archon_rewrite_graph_read(
        ArchonRewriteGraph *graph,
        ArchonGraphsNodeId node,
        ArchonGraphsNodeView *view) {
    const ArchonRewriteTermNode *term;
    size_t count, index;
    if (!graph || !view || graph->failed
            || (size_t)node >= graph->term_count)
        return ARCHON_REWRITE_INVALID_ARGUMENT;
    term = &graph->terms[node];
    count = archon_graph_predecessor_count(&graph->dfg, node);
    if (count != term->child_count
            || count > ARCHON_REWRITE_MAX_ARITY)
        return ARCHON_REWRITE_GRAPH_ERROR;
    for (index = 0u; index < count; ++index)
        graph->scratch[index] = ARCHON_REWRITE_NODE_NONE;
    for (index = 0u; index < count; ++index) {
        ArchonGraphNodeId predecessor;
        ArchonGraphEdgeId edge_id;
        const ArchonGraphEdge *edge;
        ArchonGraphStatus status = archon_graph_predecessor(
            &graph->dfg, node, index, &predecessor, &edge_id);
        if (status != ARCHON_GRAPH_OK)
            return archon_graph_status(status);
        edge = archon_graph_edge(&graph->dfg, edge_id);
        if (!edge || edge->dependency != ARCHON_DEPENDENCY_DATA
                || edge->position >= count
                || graph->scratch[edge->position]
                    != ARCHON_REWRITE_NODE_NONE)
            return ARCHON_REWRITE_GRAPH_ERROR;
        graph->scratch[edge->position] = predecessor;
    }
    for (index = 0u; index < count; ++index)
        if (graph->scratch[index] == ARCHON_REWRITE_NODE_NONE)
            return ARCHON_REWRITE_GRAPH_ERROR;
    view->opcode = term->opcode;
    view->flags = term->flags;
    view->payload = term->payload;
    view->children = count ? graph->scratch : NULL;
    view->child_count = count;
    return ARCHON_REWRITE_OK;
}

ArchonRewriteStatus archon_rewrite_graph_intern(
        ArchonRewriteGraph *graph,
        uint32_t opcode,
        uint64_t payload,
        uint32_t flags,
        const ArchonGraphsNodeId *children,
        size_t child_count,
        ArchonGraphsNodeId *result) {
    ArchonRewriteStatus status;
    ArchonGraphNodeId node;
    ArchonGraphsNodeId requested[ARCHON_REWRITE_MAX_ARITY];
    size_t index;
    if (!graph || !result || graph->failed
            || child_count > ARCHON_REWRITE_MAX_ARITY
            || (child_count && !children))
        return ARCHON_REWRITE_INVALID_ARGUMENT;
    for (index = 0u; index < child_count; ++index)
        if ((size_t)children[index] >= graph->term_count)
            return ARCHON_REWRITE_INVALID_ARGUMENT;
        else
            requested[index] = children[index];
    for (index = 0u; index < graph->term_count; ++index) {
        ArchonGraphsNodeView view;
        status = archon_rewrite_graph_read(
            graph, (ArchonGraphsNodeId)index, &view);
        if (status != ARCHON_REWRITE_OK)
            return status;
        if (view.opcode == opcode && view.payload == payload
                && view.flags == flags
                && view.child_count == child_count
                && (!child_count
                    || memcmp(view.children, requested,
                        child_count * sizeof(*requested)) == 0)) {
            *result = (ArchonGraphsNodeId)index;
            return ARCHON_REWRITE_OK;
        }
    }
    status = archon_grow_array(
        (void **)&graph->terms, &graph->term_capacity,
        graph->term_count + 1u, (size_t)UINT32_MAX,
        sizeof(*graph->terms));
    if (status != ARCHON_REWRITE_OK)
        return status;
    status = archon_graph_status(
        archon_dfg_add_value(&graph->dfg, "", &node));
    if (status != ARCHON_REWRITE_OK)
        return status;
    if ((size_t)node != graph->term_count) {
        graph->failed = 1;
        return ARCHON_REWRITE_GRAPH_ERROR;
    }
    graph->terms[graph->term_count].opcode = opcode;
    graph->terms[graph->term_count].flags = flags;
    graph->terms[graph->term_count].payload = payload;
    graph->terms[graph->term_count].child_count = child_count;
    ++graph->term_count;
    for (index = 0u; index < child_count; ++index) {
        ArchonGraphEdgeId edge;
        ArchonGraphStatus graph_status = archon_dfg_add_use(
            &graph->dfg, requested[index], node, (uint32_t)index, "", &edge);
        if (graph_status != ARCHON_GRAPH_OK) {
            graph->failed = 1;
            return archon_graph_status(graph_status);
        }
    }
    *result = node;
    return ARCHON_REWRITE_OK;
}

static int archon_rewrite_graph_read_callback(
        void *context,
        ArchonGraphsNodeId node,
        ArchonGraphsNodeView *view) {
    return archon_rewrite_graph_read(
        (ArchonRewriteGraph *)context, node, view) != ARCHON_REWRITE_OK;
}

static int archon_rewrite_graph_intern_callback(
        void *context,
        uint32_t opcode,
        uint64_t payload,
        uint32_t flags,
        const ArchonGraphsNodeId *children,
        size_t child_count,
        ArchonGraphsNodeId *result) {
    return archon_rewrite_graph_intern(
        (ArchonRewriteGraph *)context, opcode, payload, flags,
        children, child_count, result) != ARCHON_REWRITE_OK;
}

static size_t archon_rewrite_graph_count_callback(void *context) {
    const ArchonRewriteGraph *graph =
        (const ArchonRewriteGraph *)context;
    return graph ? graph->term_count : 0u;
}

ArchonGraphsAPI archon_rewrite_graph_api(ArchonRewriteGraph *graph) {
    ArchonGraphsAPI api;
    api.context = graph;
    api.read = archon_rewrite_graph_read_callback;
    api.intern = archon_rewrite_graph_intern_callback;
    api.node_count = archon_rewrite_graph_count_callback;
    api.cost = NULL;
    return api;
}

static ArchonRewriteStatus archon_apply_directed(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId start,
        const ArchonRewriteRule *rules,
        size_t rule_count,
        const ArchonRewriteLimits *limits,
        ArchonGraphsNodeId *result,
        ArchonRewriteStats *stats,
        const ArchonRewriteObserver *observer) {
    ArchonGraphsNodeId current = start;
    for (;;) {
        ArchonGraphsNodeView view;
        ArchonRewriteStatus status =
            archon_read(graphs, current, &view, limits);
        size_t i;
        int changed = 0;
        if (status != ARCHON_REWRITE_OK)
            return status;
        for (i = 0u; i < rule_count; ++i) {
            const ArchonRewriteRule *rule = &rules[i];
            ArchonGraphsNodeId replacement = current;
            if (!(rule->engines & ARCHON_REWRITE_DIRECTED)
                    || rule->lhs_opcode != view.opcode
                    || rule->arity != view.child_count)
                continue;
            if (rule->kind == ARCHON_REWRITE_RULE_RENAME) {
                ArchonGraphsNodeId children[ARCHON_REWRITE_PATTERN_ARITY];
                size_t j;
                for (j = 0u; j < rule->arity; ++j)
                    children[j] = view.children[rule->map[j]];
                if (graphs->intern(
                        graphs->context, rule->rhs_opcode, view.payload,
                        view.flags, children, rule->arity, &replacement) != 0)
                    return ARCHON_REWRITE_GRAPH_ERROR;
            } else if (rule->kind == ARCHON_REWRITE_RULE_IDENTITY) {
                ArchonGraphsNodeView constant;
                ArchonGraphsNodeId operands[2];
                unsigned side = rule->constant_side;
                operands[0] = view.children[0];
                operands[1] = view.children[1];
                status = archon_read(
                    graphs, operands[side], &constant, limits);
                if (status != ARCHON_REWRITE_OK)
                    return status;
                if (constant.opcode != rule->constant_opcode
                        || constant.payload != rule->constant_value
                        || constant.child_count != 0u)
                    continue;
                replacement = operands[1u - side];
            } else {
                continue;
            }
            if (replacement == ARCHON_REWRITE_NODE_NONE)
                return ARCHON_REWRITE_GRAPH_ERROR;
            if (replacement == current)
                continue;
            if (stats->applications >= limits->max_applications)
                return ARCHON_REWRITE_RESOURCE_LIMIT;
            ++stats->applications;
            archon_rewrite_observe(observer, i, ARCHON_REWRITE_DIRECTED);
            current = replacement;
            changed = 1;
            break;
        }
        if (!changed) {
            *result = current;
            return ARCHON_REWRITE_OK;
        }
    }
}

static ArchonRewriteStatus archon_run_directed(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId root,
        const ArchonRewriteRule *rules,
        size_t rule_count,
        const ArchonRewriteLimits *limits,
        ArchonGraphsNodeId *result,
        ArchonRewriteStats *stats,
        const ArchonRewriteObserver *observer) {
    ArchonDirectedMemo *memos = NULL;
    ArchonWorkFrame *frames = NULL;
    size_t memo_count = 0u, memo_capacity = 0u;
    size_t frame_count = 0u, frame_capacity = 0u;
    ArchonRewriteStatus status = ARCHON_REWRITE_OK;

#define ARCHON_DIRECTED_PUSH(NODE) do { \
    status = archon_grow_array((void **)&memos, &memo_capacity, \
        memo_count + 1u, limits->max_worklist, sizeof(*memos)); \
    if (status != ARCHON_REWRITE_OK) \
        goto directed_done; \
    status = archon_grow_array((void **)&frames, &frame_capacity, \
        frame_count + 1u, limits->max_worklist, sizeof(*frames)); \
    if (status != ARCHON_REWRITE_OK) \
        goto directed_done; \
    memos[memo_count].input = (NODE); \
    memos[memo_count].output = ARCHON_REWRITE_NODE_NONE; \
    memos[memo_count].state = 1u; \
    ++memo_count; \
    frames[frame_count].node = (NODE); \
    frames[frame_count].next_child = 0u; \
    frames[frame_count].expanded = 0u; \
    ++frame_count; \
} while (0)

    ARCHON_DIRECTED_PUSH(root);
    while (frame_count) {
        ArchonWorkFrame *frame = &frames[frame_count - 1u];
        size_t memo_index = archon_directed_find(
            memos, memo_count, frame->node);
        ArchonGraphsNodeView view;
        if (memo_index == SIZE_MAX) {
            status = ARCHON_REWRITE_GRAPH_ERROR;
            goto directed_done;
        }
        status = archon_read(graphs, frame->node, &view, limits);
        if (status != ARCHON_REWRITE_OK)
            goto directed_done;
        if (!frame->expanded) {
            frame->expanded = 1u;
            frame->next_child = 0u;
        }
        if (frame->next_child < view.child_count) {
            ArchonGraphsNodeId child =
                view.children[frame->next_child++];
            size_t child_memo = archon_directed_find(
                memos, memo_count, child);
            if (child_memo != SIZE_MAX) {
                if (memos[child_memo].state != 2u) {
                    status = ARCHON_REWRITE_CYCLIC_TERM;
                    goto directed_done;
                }
            } else {
                ARCHON_DIRECTED_PUSH(child);
            }
            continue;
        } else {
            ArchonGraphsNodeId children[ARCHON_REWRITE_MAX_ARITY];
            ArchonGraphsNodeId rebuilt;
            size_t i;
            for (i = 0u; i < view.child_count; ++i) {
                size_t child_memo = archon_directed_find(
                    memos, memo_count, view.children[i]);
                if (child_memo == SIZE_MAX
                        || memos[child_memo].state != 2u) {
                    status = ARCHON_REWRITE_GRAPH_ERROR;
                    goto directed_done;
                }
                children[i] = memos[child_memo].output;
            }
            if (graphs->intern(
                    graphs->context, view.opcode, view.payload, view.flags,
                    children, view.child_count, &rebuilt) != 0) {
                status = ARCHON_REWRITE_GRAPH_ERROR;
                goto directed_done;
            }
            status = archon_apply_directed(
                graphs, rebuilt, rules, rule_count, limits,
                &memos[memo_index].output, stats, observer);
            if (status != ARCHON_REWRITE_OK)
                goto directed_done;
            memos[memo_index].state = 2u;
            --frame_count;
        }
    }
    {
        size_t root_memo = archon_directed_find(memos, memo_count, root);
        if (root_memo == SIZE_MAX || memos[root_memo].state != 2u)
            status = ARCHON_REWRITE_GRAPH_ERROR;
        else
            *result = memos[root_memo].output;
    }
    stats->iterations = 1u;
    stats->saturated = status == ARCHON_REWRITE_OK;

directed_done:
#undef ARCHON_DIRECTED_PUSH
    free(frames);
    free(memos);
    return status;
}

static uint32_t archon_egraph_find(ArchonEGraph *graph, uint32_t id) {
    uint32_t root = id;
    while (graph->classes[root].parent != root)
        root = graph->classes[root].parent;
    while (graph->classes[id].parent != id) {
        uint32_t next = graph->classes[id].parent;
        graph->classes[id].parent = root;
        id = next;
    }
    return root;
}

static int archon_egraph_union(
        ArchonEGraph *graph, uint32_t left, uint32_t right) {
    uint32_t a = archon_egraph_find(graph, left);
    uint32_t b = archon_egraph_find(graph, right);
    uint32_t low, high;
    if (a == b)
        return 0;
    low = a < b ? a : b;
    high = a < b ? b : a;
    graph->classes[high].parent = low;
    return 1;
}

static uint64_t archon_hash_mix(uint64_t hash, uint64_t value) {
    hash ^= value + UINT64_C(0x9e3779b97f4a7c15)
        + (hash << 6u) + (hash >> 2u);
    return hash;
}

static uint64_t archon_enode_hash(const ArchonENode *node) {
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    size_t i;
    hash = archon_hash_mix(hash, node->opcode);
    hash = archon_hash_mix(hash, node->flags);
    hash = archon_hash_mix(hash, node->payload);
    hash = archon_hash_mix(hash, node->arity);
    for (i = 0u; i < node->arity; ++i)
        hash = archon_hash_mix(hash, node->children[i]);
    return hash;
}

static int archon_enode_equal(
        const ArchonENode *left, const ArchonENode *right) {
    size_t i;
    if (left->opcode != right->opcode || left->flags != right->flags
            || left->payload != right->payload
            || left->arity != right->arity)
        return 0;
    for (i = 0u; i < left->arity; ++i)
        if (left->children[i] != right->children[i])
            return 0;
    return 1;
}

static ArchonRewriteStatus archon_egraph_init(
        ArchonEGraph *graph, const ArchonRewriteLimits *limits) {
    size_t hash_capacity = 16u, wanted;
    memset(graph, 0, sizeof(*graph));
    if (archon_size_multiply_overflows(
            limits->max_enodes, sizeof(*graph->nodes))
            || archon_size_multiply_overflows(
                limits->max_eclasses, sizeof(*graph->classes))
            || limits->max_enodes > (SIZE_MAX - 1u) / 2u)
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    wanted = limits->max_enodes * 2u + 1u;
    while (hash_capacity < wanted) {
        if (hash_capacity > SIZE_MAX / 2u)
            return ARCHON_REWRITE_RESOURCE_LIMIT;
        hash_capacity *= 2u;
    }
    if (archon_size_multiply_overflows(
            hash_capacity, sizeof(*graph->hash_table)))
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    graph->nodes = (ArchonENode *)calloc(
        limits->max_enodes, sizeof(*graph->nodes));
    graph->classes = (ArchonEClass *)calloc(
        limits->max_eclasses, sizeof(*graph->classes));
    graph->hash_table = (uint32_t *)calloc(
        hash_capacity, sizeof(*graph->hash_table));
    if (!graph->nodes || !graph->classes || !graph->hash_table) {
        free(graph->nodes);
        free(graph->classes);
        free(graph->hash_table);
        memset(graph, 0, sizeof(*graph));
        return ARCHON_REWRITE_OUT_OF_MEMORY;
    }
    graph->node_capacity = limits->max_enodes;
    graph->class_capacity = limits->max_eclasses;
    graph->hash_capacity = hash_capacity;
    return ARCHON_REWRITE_OK;
}

static void archon_egraph_destroy(ArchonEGraph *graph) {
    free(graph->nodes);
    free(graph->classes);
    free(graph->hash_table);
    memset(graph, 0, sizeof(*graph));
}

static size_t archon_egraph_slot(
        const ArchonEGraph *graph, const ArchonENode *candidate, int *found) {
    size_t mask = graph->hash_capacity - 1u;
    size_t slot = (size_t)archon_enode_hash(candidate) & mask;
    for (;;) {
        uint32_t stored = graph->hash_table[slot];
        if (!stored) {
            *found = 0;
            return slot;
        }
        if (archon_enode_equal(&graph->nodes[stored - 1u], candidate)) {
            *found = 1;
            return slot;
        }
        slot = (slot + 1u) & mask;
    }
}

static ArchonRewriteStatus archon_egraph_add(
        ArchonEGraph *graph,
        uint32_t opcode,
        uint64_t payload,
        uint32_t flags,
        const uint32_t *children,
        size_t arity,
        uint32_t *result,
        int *was_new) {
    ArchonENode candidate;
    size_t i, slot;
    int found;
    if (arity > ARCHON_REWRITE_MAX_ARITY)
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    memset(&candidate, 0, sizeof(candidate));
    candidate.opcode = opcode;
    candidate.payload = payload;
    candidate.flags = flags;
    candidate.arity = (uint8_t)arity;
    for (i = 0u; i < arity; ++i) {
        if ((size_t)children[i] >= graph->class_count)
            return ARCHON_REWRITE_GRAPH_ERROR;
        candidate.children[i] = archon_egraph_find(graph, children[i]);
    }
    slot = archon_egraph_slot(graph, &candidate, &found);
    if (found) {
        uint32_t node = graph->hash_table[slot] - 1u;
        *result = archon_egraph_find(graph, graph->nodes[node].eclass);
        *was_new = 0;
        return ARCHON_REWRITE_OK;
    }
    if (graph->node_count >= graph->node_capacity
            || graph->class_count >= graph->class_capacity)
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    candidate.eclass = (uint32_t)graph->class_count;
    graph->classes[graph->class_count].parent =
        (uint32_t)graph->class_count;
    ++graph->class_count;
    graph->nodes[graph->node_count] = candidate;
    graph->hash_table[slot] = (uint32_t)graph->node_count + 1u;
    ++graph->node_count;
    *result = candidate.eclass;
    *was_new = 1;
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_egraph_rebuild(ArchonEGraph *graph) {
    int merged;
    size_t passes = 0u;
    do {
        size_t i, j;
        merged = 0;
        memset(graph->hash_table, 0,
               graph->hash_capacity * sizeof(*graph->hash_table));
        for (i = 0u; i < graph->node_count; ++i) {
            ArchonENode *node = &graph->nodes[i];
            int found;
            size_t slot;
            node->eclass = archon_egraph_find(graph, node->eclass);
            for (j = 0u; j < node->arity; ++j)
                node->children[j] =
                    archon_egraph_find(graph, node->children[j]);
            slot = archon_egraph_slot(graph, node, &found);
            if (found) {
                uint32_t other = graph->hash_table[slot] - 1u;
                if (archon_egraph_union(
                        graph, node->eclass,
                        graph->nodes[other].eclass))
                    merged = 1;
            } else {
                graph->hash_table[slot] = (uint32_t)i + 1u;
            }
        }
        ++passes;
        if (passes > graph->class_count + 1u)
            return ARCHON_REWRITE_GRAPH_ERROR;
    } while (merged);
    return ARCHON_REWRITE_OK;
}

static int archon_eclass_has_constant(
        ArchonEGraph *graph,
        uint32_t eclass,
        uint32_t opcode,
        uint64_t value) {
    uint32_t root = archon_egraph_find(graph, eclass);
    size_t i;
    for (i = 0u; i < graph->node_count; ++i) {
        ArchonENode *node = &graph->nodes[i];
        if (archon_egraph_find(graph, node->eclass) == root
                && node->opcode == opcode && node->payload == value
                && node->arity == 0u)
            return 1;
    }
    return 0;
}

static ArchonRewriteStatus archon_import_term(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId root,
        ArchonEGraph *egraph,
        const ArchonRewriteLimits *limits,
        uint32_t *root_class) {
    ArchonImportMemo *memos = NULL;
    ArchonWorkFrame *frames = NULL;
    size_t memo_count = 0u, memo_capacity = 0u;
    size_t frame_count = 0u, frame_capacity = 0u;
    ArchonRewriteStatus status = ARCHON_REWRITE_OK;

#define ARCHON_IMPORT_PUSH(NODE) do { \
    status = archon_grow_array((void **)&memos, &memo_capacity, \
        memo_count + 1u, limits->max_worklist, sizeof(*memos)); \
    if (status != ARCHON_REWRITE_OK) \
        goto import_done; \
    status = archon_grow_array((void **)&frames, &frame_capacity, \
        frame_count + 1u, limits->max_worklist, sizeof(*frames)); \
    if (status != ARCHON_REWRITE_OK) \
        goto import_done; \
    memos[memo_count].input = (NODE); \
    memos[memo_count].eclass = UINT32_MAX; \
    memos[memo_count].state = 1u; \
    ++memo_count; \
    frames[frame_count].node = (NODE); \
    frames[frame_count].next_child = 0u; \
    frames[frame_count].expanded = 0u; \
    ++frame_count; \
} while (0)

    ARCHON_IMPORT_PUSH(root);
    while (frame_count) {
        ArchonWorkFrame *frame = &frames[frame_count - 1u];
        size_t memo_index = archon_import_find(
            memos, memo_count, frame->node);
        ArchonGraphsNodeView view;
        if (memo_index == SIZE_MAX) {
            status = ARCHON_REWRITE_GRAPH_ERROR;
            goto import_done;
        }
        status = archon_read(graphs, frame->node, &view, limits);
        if (status != ARCHON_REWRITE_OK)
            goto import_done;
        if (!frame->expanded) {
            frame->expanded = 1u;
            frame->next_child = 0u;
        }
        if (frame->next_child < view.child_count) {
            ArchonGraphsNodeId child =
                view.children[frame->next_child++];
            size_t child_memo = archon_import_find(
                memos, memo_count, child);
            if (child_memo != SIZE_MAX) {
                if (memos[child_memo].state != 2u) {
                    status = ARCHON_REWRITE_CYCLIC_TERM;
                    goto import_done;
                }
            } else {
                ARCHON_IMPORT_PUSH(child);
            }
            continue;
        } else {
            uint32_t children[ARCHON_REWRITE_MAX_ARITY];
            uint32_t eclass;
            int was_new;
            size_t i;
            for (i = 0u; i < view.child_count; ++i) {
                size_t child_memo = archon_import_find(
                    memos, memo_count, view.children[i]);
                if (child_memo == SIZE_MAX
                        || memos[child_memo].state != 2u) {
                    status = ARCHON_REWRITE_GRAPH_ERROR;
                    goto import_done;
                }
                children[i] = memos[child_memo].eclass;
            }
            status = archon_egraph_add(
                egraph, view.opcode, view.payload, view.flags,
                children, view.child_count, &eclass, &was_new);
            if (status != ARCHON_REWRITE_OK)
                goto import_done;
            (void)was_new;
            memos[memo_index].eclass = eclass;
            memos[memo_index].state = 2u;
            --frame_count;
        }
    }
    {
        size_t root_memo = archon_import_find(memos, memo_count, root);
        if (root_memo == SIZE_MAX || memos[root_memo].state != 2u)
            status = ARCHON_REWRITE_GRAPH_ERROR;
        else
            *root_class = memos[root_memo].eclass;
    }

import_done:
#undef ARCHON_IMPORT_PUSH
    free(frames);
    free(memos);
    return status;
}

static ArchonRewriteStatus archon_saturate(
        ArchonEGraph *graph,
        const ArchonRewriteRule *rules,
        size_t rule_count,
        const ArchonRewriteLimits *limits,
        ArchonRewriteStats *stats,
        const ArchonRewriteObserver *observer) {
    size_t iteration;
    ArchonRewriteStatus status = archon_egraph_rebuild(graph);
    if (status != ARCHON_REWRITE_OK)
        return status;
    for (iteration = 0u; iteration < limits->max_iterations; ++iteration) {
        size_t initial_nodes = graph->node_count;
        size_t node_index;
        int changed = 0;
        for (node_index = 0u; node_index < initial_nodes; ++node_index) {
            ArchonENode node = graph->nodes[node_index];
            size_t rule_index;
            node.eclass = archon_egraph_find(graph, node.eclass);
            for (rule_index = 0u; rule_index < rule_count; ++rule_index) {
                const ArchonRewriteRule *rule = &rules[rule_index];
                uint32_t replacement = node.eclass;
                int was_new = 0, merged = 0, applied = 0;
                if (!(rule->engines & ARCHON_REWRITE_EQUALITY_SATURATION)
                        || rule->lhs_opcode != node.opcode
                        || rule->arity != node.arity)
                    continue;
                if (rule->kind == ARCHON_REWRITE_RULE_RENAME) {
                    uint32_t children[ARCHON_REWRITE_PATTERN_ARITY];
                    size_t i;
                    for (i = 0u; i < rule->arity; ++i)
                        children[i] = node.children[rule->map[i]];
                    status = archon_egraph_add(
                        graph, rule->rhs_opcode, node.payload, node.flags,
                        children, rule->arity, &replacement, &was_new);
                    if (status != ARCHON_REWRITE_OK)
                        return status;
                    merged = archon_egraph_union(
                        graph, node.eclass, replacement);
                    applied = was_new || merged;
                } else if (rule->kind
                        == ARCHON_REWRITE_RULE_IDENTITY) {
                    unsigned side = rule->constant_side;
                    if (!archon_eclass_has_constant(
                            graph, node.children[side],
                            rule->constant_opcode,
                            rule->constant_value))
                        continue;
                    replacement = node.children[1u - side];
                    merged = archon_egraph_union(
                        graph, node.eclass, replacement);
                    applied = merged;
                } else if (rule->kind
                        == ARCHON_REWRITE_RULE_COMMUTATIVE) {
                    uint32_t children[2];
                    children[0] = node.children[1];
                    children[1] = node.children[0];
                    status = archon_egraph_add(
                        graph, node.opcode, node.payload, node.flags,
                        children, 2u, &replacement, &was_new);
                    if (status != ARCHON_REWRITE_OK)
                        return status;
                    merged = archon_egraph_union(
                        graph, node.eclass, replacement);
                    applied = was_new || merged;
                }
                if (applied) {
                    if (stats->applications >= limits->max_applications)
                        return ARCHON_REWRITE_RESOURCE_LIMIT;
                    ++stats->applications;
                    archon_rewrite_observe(
                        observer, rule_index, ARCHON_REWRITE_EQUALITY_SATURATION);
                    changed = 1;
                }
            }
        }
        status = archon_egraph_rebuild(graph);
        if (status != ARCHON_REWRITE_OK)
            return status;
        stats->iterations = iteration + 1u;
        if (!changed) {
            stats->saturated = 1;
            return ARCHON_REWRITE_OK;
        }
    }
    stats->saturated = 0;
    return ARCHON_REWRITE_RESOURCE_LIMIT;
}

static uint64_t archon_generated_cost(uint32_t opcode) {
    size_t i;
    for (i = 0u; i < archon_generated_target_fact_count; ++i)
        if (archon_generated_target_facts[i].opcode == opcode)
            return archon_generated_target_facts[i].cost;
    return UINT64_C(1);
}

static uint64_t archon_node_cost(
        const ArchonGraphsAPI *graphs,
        const ArchonENode *node,
        const ArchonRewriteRule *rules,
        size_t rule_count) {
    uint64_t cost = ARCHON_COST_INFINITY;
    size_t i;
    if (graphs->cost)
        return graphs->cost(
            graphs->context, node->opcode, node->payload, node->flags);
    for (i = 0u; i < rule_count; ++i) {
        const ArchonRewriteRule *rule = &rules[i];
        if (rule->lhs_opcode == node->opcode
                && (uint64_t)rule->lhs_cost < cost)
            cost = rule->lhs_cost;
        if (rule->kind == ARCHON_REWRITE_RULE_RENAME
                && rule->rhs_opcode == node->opcode
                && (uint64_t)rule->rhs_cost < cost)
            cost = rule->rhs_cost;
    }
    if (cost != ARCHON_COST_INFINITY)
        return cost;
    return archon_generated_cost(node->opcode);
}

static ArchonRewriteStatus archon_extract(
        ArchonEGraph *graph,
        uint32_t root_class,
        const ArchonGraphsAPI *graphs,
        const ArchonRewriteRule *rules,
        size_t rule_count,
        const ArchonRewriteLimits *limits,
        ArchonGraphsNodeId *result) {
    uint64_t *costs = NULL;
    uint32_t *best = NULL;
    ArchonGraphsNodeId *outputs = NULL;
    uint8_t *states = NULL;
    uint32_t *class_stack = NULL;
    size_t *next_children = NULL;
    uint8_t *expanded = NULL;
    size_t stack_count = 0u, pass, i;
    ArchonRewriteStatus status = ARCHON_REWRITE_OK;
    if (archon_size_multiply_overflows(
            graph->class_count, sizeof(*costs))
            || archon_size_multiply_overflows(
                graph->class_count, sizeof(*best))
            || archon_size_multiply_overflows(
                graph->class_count, sizeof(*outputs))
            || archon_size_multiply_overflows(
                graph->class_count, sizeof(*states))
            || archon_size_multiply_overflows(
                graph->class_count, sizeof(*next_children)))
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    costs = (uint64_t *)malloc(graph->class_count * sizeof(*costs));
    best = (uint32_t *)malloc(graph->class_count * sizeof(*best));
    outputs = (ArchonGraphsNodeId *)malloc(
        graph->class_count * sizeof(*outputs));
    states = (uint8_t *)calloc(graph->class_count, sizeof(*states));
    class_stack = (uint32_t *)malloc(
        graph->class_count * sizeof(*class_stack));
    next_children = (size_t *)malloc(
        graph->class_count * sizeof(*next_children));
    expanded = (uint8_t *)malloc(graph->class_count * sizeof(*expanded));
    if (!costs || !best || !outputs || !states || !class_stack
            || !next_children || !expanded) {
        status = ARCHON_REWRITE_OUT_OF_MEMORY;
        goto extract_done;
    }
    for (i = 0u; i < graph->class_count; ++i) {
        costs[i] = ARCHON_COST_INFINITY;
        best[i] = UINT32_MAX;
        outputs[i] = ARCHON_REWRITE_NODE_NONE;
    }
    for (pass = 0u; pass <= graph->class_count; ++pass) {
        int changed = 0;
        size_t node_index;
        for (node_index = 0u; node_index < graph->node_count; ++node_index) {
            ArchonENode *node = &graph->nodes[node_index];
            uint32_t owner = archon_egraph_find(graph, node->eclass);
            uint64_t cost = archon_node_cost(
                graphs, node, rules, rule_count);
            size_t child;
            if (cost == ARCHON_COST_INFINITY)
                continue;
            for (child = 0u; child < node->arity; ++child) {
                uint32_t child_class =
                    archon_egraph_find(graph, node->children[child]);
                if (costs[child_class] == ARCHON_COST_INFINITY
                        || UINT64_MAX - cost < costs[child_class]) {
                    cost = ARCHON_COST_INFINITY;
                    break;
                }
                cost += costs[child_class];
            }
            if (cost == ARCHON_COST_INFINITY)
                continue;
            /*
             * Keep the first representative at equal cost.  A finite
             * representative can only be discovered from already-finite
             * children, so first discovery builds an acyclic extraction DAG.
             * Replacing it with an equal-cost recursive representative can
             * otherwise manufacture a cycle after identity unions.  E-nodes
             * are inserted deterministically, making this tie policy stable.
             */
            if (cost < costs[owner]) {
                costs[owner] = cost;
                best[owner] = (uint32_t)node_index;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }
    root_class = archon_egraph_find(graph, root_class);
    if (best[root_class] == UINT32_MAX) {
        status = ARCHON_REWRITE_NO_EXTRACTION;
        goto extract_done;
    }
    class_stack[stack_count] = root_class;
    next_children[stack_count] = 0u;
    expanded[stack_count++] = 0u;
    states[root_class] = 1u;
    while (stack_count) {
        uint32_t owner = class_stack[stack_count - 1u];
        uint32_t node_index = best[owner];
        ArchonENode *node;
        if (node_index == UINT32_MAX) {
            status = ARCHON_REWRITE_NO_EXTRACTION;
            goto extract_done;
        }
        node = &graph->nodes[node_index];
        if (!expanded[stack_count - 1u]) {
            expanded[stack_count - 1u] = 1u;
            next_children[stack_count - 1u] = 0u;
        }
        if (next_children[stack_count - 1u] < node->arity) {
            size_t child = next_children[stack_count - 1u]++;
            uint32_t child_class =
                archon_egraph_find(graph, node->children[child]);
            if (states[child_class] == 1u) {
                status = ARCHON_REWRITE_NO_EXTRACTION;
                goto extract_done;
            }
            if (states[child_class] != 2u) {
                if (stack_count >= graph->class_count
                        || stack_count >= limits->max_worklist) {
                    status = ARCHON_REWRITE_RESOURCE_LIMIT;
                    goto extract_done;
                }
                states[child_class] = 1u;
                class_stack[stack_count] = child_class;
                next_children[stack_count] = 0u;
                expanded[stack_count++] = 0u;
            }
            continue;
        } else {
            ArchonGraphsNodeId children[ARCHON_REWRITE_MAX_ARITY];
            ArchonGraphsNodeId output;
            size_t child;
            for (child = 0u; child < node->arity; ++child) {
                uint32_t child_class =
                    archon_egraph_find(graph, node->children[child]);
                if (states[child_class] != 2u) {
                    status = ARCHON_REWRITE_NO_EXTRACTION;
                    goto extract_done;
                }
                children[child] = outputs[child_class];
            }
            if (graphs->intern(
                    graphs->context, node->opcode, node->payload, node->flags,
                    children, node->arity, &output) != 0
                    || output == ARCHON_REWRITE_NODE_NONE) {
                status = ARCHON_REWRITE_GRAPH_ERROR;
                goto extract_done;
            }
            outputs[owner] = output;
            states[owner] = 2u;
            --stack_count;
        }
    }
    *result = outputs[root_class];

extract_done:
    free(expanded);
    free(next_children);
    free(class_stack);
    free(states);
    free(outputs);
    free(best);
    free(costs);
    return status;
}

static ArchonRewriteStatus archon_run_equality(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId root,
        const ArchonRewriteRule *rules,
        size_t rule_count,
        const ArchonRewriteLimits *limits,
        ArchonGraphsNodeId *result,
        ArchonRewriteStats *stats,
        const ArchonRewriteObserver *observer) {
    ArchonEGraph graph;
    uint32_t root_class = UINT32_MAX;
    ArchonRewriteStatus status = archon_egraph_init(&graph, limits);
    if (status != ARCHON_REWRITE_OK)
        return status;
    status = archon_import_term(
        graphs, root, &graph, limits, &root_class);
    if (status == ARCHON_REWRITE_OK)
        status = archon_saturate(
            &graph, rules, rule_count, limits, stats, observer);
    /*
     * A resource-bound saturation still has a valid rebuilt e-graph, but an
     * explicit limit status must not be silently reported as a proof of
     * saturation.  Do not extract in that case.
     */
    if (status == ARCHON_REWRITE_OK)
        status = archon_extract(
            &graph, root_class, graphs, rules, rule_count, limits, result);
    stats->enodes = graph.node_count;
    stats->eclasses = graph.class_count;
    archon_egraph_destroy(&graph);
    return status;
}

static ArchonRewriteStatus archon_rewrite_run_impl(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId root,
        ArchonRewriteEngine engine,
        const ArchonRewriteRule *rules,
        size_t rule_count,
        const ArchonRewriteLimits *limits_argument,
        ArchonGraphsNodeId *result,
        ArchonRewriteStats *stats_argument,
        const ArchonRewriteObserver *observer) {
    ArchonRewriteLimits defaults;
    const ArchonRewriteLimits *limits = limits_argument;
    ArchonRewriteStats local_stats;
    ArchonRewriteStats *stats = stats_argument ? stats_argument : &local_stats;
    ArchonRewriteStatus status;
    if (!limits) {
        defaults = archon_rewrite_default_limits();
        limits = &defaults;
    }
    memset(stats, 0, sizeof(*stats));
    if (!graphs || !graphs->read || !graphs->intern || !result
            || root == ARCHON_REWRITE_NODE_NONE
            || (engine != ARCHON_REWRITE_DIRECTED
                && engine != ARCHON_REWRITE_EQUALITY_SATURATION))
        return ARCHON_REWRITE_INVALID_ARGUMENT;
    status = archon_validate_limits(limits);
    if (status != ARCHON_REWRITE_OK)
        return status;
    status = archon_validate_rules(rules, rule_count);
    if (status != ARCHON_REWRITE_OK)
        return status;
    if (graphs->node_count
            && graphs->node_count(graphs->context) > limits->max_worklist)
        return ARCHON_REWRITE_RESOURCE_LIMIT;
    if (engine == ARCHON_REWRITE_DIRECTED)
        return archon_run_directed(
            graphs, root, rules, rule_count, limits, result, stats, observer);
    return archon_run_equality(
        graphs, root, rules, rule_count, limits, result, stats, observer);
}

ArchonRewriteStatus archon_rewrite_run(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId root,
        ArchonRewriteEngine engine,
        const ArchonRewriteRule *rules,
        size_t rule_count,
        const ArchonRewriteLimits *limits,
        ArchonGraphsNodeId *result,
        ArchonRewriteStats *stats) {
    return archon_rewrite_run_impl(
        graphs, root, engine, rules, rule_count, limits, result, stats, NULL);
}

ArchonRewriteStatus archon_rewrite_run_generated_observed(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId root,
        ArchonRewriteEngine engine,
        const ArchonRewriteLimits *limits,
        ArchonGraphsNodeId *result,
        ArchonRewriteStats *stats,
        ArchonRewriteApplicationFn observe,
        void *user) {
    ArchonRewriteObserver observer = {observe, user};
    return archon_rewrite_run_impl(
        graphs, root, engine, archon_generated_rewrite_rules,
        archon_generated_rewrite_rule_count, limits, result, stats, &observer);
}

ArchonRewriteStatus archon_rewrite_run_generated(
        const ArchonGraphsAPI *graphs,
        ArchonGraphsNodeId root,
        ArchonRewriteEngine engine,
        const ArchonRewriteLimits *limits,
        ArchonGraphsNodeId *result,
        ArchonRewriteStats *stats) {
    return archon_rewrite_run(
        graphs, root, engine, archon_generated_rewrite_rules,
        archon_generated_rewrite_rule_count, limits, result, stats);
}

#if defined(ARCHON_REWRITE_INCLUDE_PROGRAM) || defined(ARCHON_REWRITE_PROGRAM_FILE)
typedef struct ArchonProgramNode {
    uint32_t opcode;
    uint32_t flags;
    uint64_t payload;
    ArchonGraphsNodeId mapped;
    ArchonGraphsNodeId inputs[ARCHON_REWRITE_MAX_ARITY];
    uint8_t input_kinds[ARCHON_REWRITE_MAX_ARITY];
    size_t arity;
    size_t supplied;
    int defined;
} ArchonProgramNode;

typedef struct ArchonProgramState {
    const ArchonGraphsAPI *graphs;
    ArchonProgramNode *nodes;
    size_t node_count;
    size_t input_count;
    size_t supplied_inputs;
    ArchonGraphsNodeId root;
    unsigned root_seen;
    unsigned begun;
    unsigned ended;
} ArchonProgramState;

static void archon_program_dispose(ArchonProgramState *state) {
    free(state->nodes);
    memset(state, 0, sizeof(*state));
}

static ArchonRewriteStatus archon_program_begin(
        ArchonProgramState *state,
        const ArchonGraphsAPI *graphs,
        const ArchonRewriteLimits *limits,
        unsigned schema,
        size_t nodes,
        size_t inputs) {
    if (!state || state->begun || schema != 1u || !nodes
            || nodes > limits->max_worklist
            || limits->max_worklist > SIZE_MAX / limits->max_arity
            || inputs > limits->max_worklist * limits->max_arity
            || archon_size_multiply_overflows(nodes, sizeof(*state->nodes)))
        return ARCHON_REWRITE_MALFORMED_PROGRAM;
    state->nodes = nodes
        ? (ArchonProgramNode *)calloc(nodes, sizeof(*state->nodes)) : NULL;
    if (nodes && !state->nodes)
        return ARCHON_REWRITE_OUT_OF_MEMORY;
    state->graphs = graphs;
    state->node_count = nodes;
    state->input_count = inputs;
    state->root = ARCHON_REWRITE_NODE_NONE;
    state->begun = 1u;
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_program_node(
        ArchonProgramState *state,
        ArchonGraphsNodeId id,
        unsigned kind,
        uint32_t opcode,
        uint32_t flags,
        ArchonGraphsNodeId region,
        uint64_t payload,
        size_t arity,
        const ArchonRewriteLimits *limits) {
    ArchonProgramNode *node;
    size_t i;
    /*
     * Term rewriting cannot erase Sea-of-Nodes region/control semantics.
     * Generic and constant, region-free data nodes are the safe subset.
     */
    if (!state || !state->begun || state->ended
            || (size_t)id >= state->node_count
            || state->nodes[id].defined
            || (kind != 0u && kind != 7u)
            || (flags & ~UINT32_C(0x0f)) != 0u
            || region != ARCHON_REWRITE_NODE_NONE
            || (kind == 7u && arity != 0u)
            || arity > limits->max_arity
            || arity > ARCHON_REWRITE_MAX_ARITY)
        return ARCHON_REWRITE_MALFORMED_PROGRAM;
    node = &state->nodes[id];
    node->opcode = opcode;
    node->flags = 0u;
    if (flags & (1u << 0))
        node->flags |= ARCHON_GRAPHS_FLAG_PURE;
    if (flags & (1u << 1))
        node->flags |= ARCHON_GRAPHS_FLAG_PINNED;
    if (flags & (1u << 2))
        node->flags |= ARCHON_GRAPHS_FLAG_MAY_TRAP;
    if (flags & (1u << 3))
        node->flags |= ARCHON_GRAPHS_FLAG_TERMINATOR;
    if (kind == 7u)
        node->flags |= ARCHON_GRAPHS_FLAG_CONSTANT
            | ARCHON_GRAPHS_FLAG_PURE;
    node->payload = payload;
    node->mapped = ARCHON_REWRITE_NODE_NONE;
    node->arity = arity;
    node->defined = 1;
    for (i = 0u; i < arity; ++i)
        node->inputs[i] = ARCHON_REWRITE_NODE_NONE;
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_program_input(
        ArchonProgramState *state,
        ArchonGraphsNodeId id,
        size_t slot,
        unsigned kind,
        ArchonGraphsNodeId source) {
    ArchonProgramNode *node;
    if (!state || !state->begun || state->ended
            || (size_t)id >= state->node_count
            || (size_t)source >= state->node_count
            || !state->nodes[id].defined || kind != 1u)
        return ARCHON_REWRITE_MALFORMED_PROGRAM;
    node = &state->nodes[id];
    if (slot >= node->arity
            || node->inputs[slot] != ARCHON_REWRITE_NODE_NONE)
        return ARCHON_REWRITE_MALFORMED_PROGRAM;
    node->inputs[slot] = source;
    node->input_kinds[slot] = (uint8_t)kind;
    ++node->supplied;
    ++state->supplied_inputs;
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_program_root(
        ArchonProgramState *state, ArchonGraphsNodeId root) {
    if (!state || !state->begun || state->ended || state->root_seen
            || (size_t)root >= state->node_count)
        return ARCHON_REWRITE_MALFORMED_PROGRAM;
    state->root = root;
    state->root_seen = 1u;
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_program_end(
        ArchonProgramState *state) {
    size_t i, j;
    if (!state || !state->begun || state->ended
            || state->supplied_inputs != state->input_count
            || (state->node_count && !state->root_seen))
        return ARCHON_REWRITE_MALFORMED_PROGRAM;
    for (i = 0u; i < state->node_count; ++i) {
        ArchonProgramNode *node = &state->nodes[i];
        if (!node->defined || node->supplied != node->arity)
            return ARCHON_REWRITE_MALFORMED_PROGRAM;
        for (j = 0u; j < node->arity; ++j) {
            ArchonGraphsNodeId source = node->inputs[j];
            if ((size_t)source >= i)
                return ARCHON_REWRITE_MALFORMED_PROGRAM;
        }
    }
    state->ended = 1u;
    return ARCHON_REWRITE_OK;
}

static ArchonRewriteStatus archon_program_materialize(
        ArchonProgramState *state) {
    size_t i, j;
    if (!state || !state->ended)
        return ARCHON_REWRITE_MALFORMED_PROGRAM;
    for (i = 0u; i < state->node_count; ++i) {
        ArchonProgramNode *node = &state->nodes[i];
        ArchonGraphsNodeId children[ARCHON_REWRITE_MAX_ARITY];
        for (j = 0u; j < node->arity; ++j) {
            ArchonGraphsNodeId source = node->inputs[j];
            if (state->nodes[source].mapped == ARCHON_REWRITE_NODE_NONE)
                return ARCHON_REWRITE_GRAPH_ERROR;
            children[j] = state->nodes[source].mapped;
        }
        if (state->graphs->intern(
                state->graphs->context, node->opcode, node->payload,
                node->flags, children, node->arity, &node->mapped) != 0
                || node->mapped == ARCHON_REWRITE_NODE_NONE)
            return ARCHON_REWRITE_GRAPH_ERROR;
    }
    return ARCHON_REWRITE_OK;
}
#endif

ArchonRewriteStatus archon_rewrite_program(
        const ArchonGraphsAPI *graphs,
        ArchonRewriteEngine engine,
        const ArchonRewriteLimits *limits_argument,
        ArchonGraphsNodeId *result,
        ArchonRewriteStats *stats) {
#if defined(ARCHON_REWRITE_INCLUDE_PROGRAM) || defined(ARCHON_REWRITE_PROGRAM_FILE)
    ArchonRewriteLimits defaults;
    const ArchonRewriteLimits *limits = limits_argument;
    ArchonProgramState state;
    ArchonRewriteStatus status = ARCHON_REWRITE_OK;
    if (!limits) {
        defaults = archon_rewrite_default_limits();
        limits = &defaults;
    }
    if (!graphs || !graphs->read || !graphs->intern || !result)
        return ARCHON_REWRITE_INVALID_ARGUMENT;
    status = archon_validate_limits(limits);
    if (status != ARCHON_REWRITE_OK)
        return status;
    memset(&state, 0, sizeof(state));
    /*
     * A valid stream may have no inputs, while a malformed stream may omit
     * any callback.  Keep every decoder helper referenced so strict builds do
     * not depend on which X-macros happen to occur in Program.i.
     */
    (void)&archon_program_begin;
    (void)&archon_program_node;
    (void)&archon_program_input;
    (void)&archon_program_root;
    (void)&archon_program_end;
/*
 * Let a hand-written Sea-of-Nodes stream use the public symbolic spellings
 * without coupling Rewrite.c to a particular generated IR.h.
 */
#ifndef ARCHON_IR_PROGRAM_SCHEMA
#define ARCHON_IR_PROGRAM_SCHEMA 1u
#define ARCHON_REWRITE_UNDEF_IR_PROGRAM_SCHEMA
#endif
#ifndef ARCHON_IR_NODE_NONE
#define ARCHON_IR_NODE_NONE UINT32_MAX
#define ARCHON_REWRITE_UNDEF_IR_NODE_NONE
#endif
#ifndef ARCHON_IR_NODE_GENERIC
#define ARCHON_IR_NODE_GENERIC 0u
#define ARCHON_REWRITE_UNDEF_IR_NODE_GENERIC
#endif
#ifndef ARCHON_IR_NODE_START
#define ARCHON_IR_NODE_START 1u
#define ARCHON_REWRITE_UNDEF_IR_NODE_START
#endif
#ifndef ARCHON_IR_NODE_END
#define ARCHON_IR_NODE_END 2u
#define ARCHON_REWRITE_UNDEF_IR_NODE_END
#endif
#ifndef ARCHON_IR_NODE_REGION
#define ARCHON_IR_NODE_REGION 3u
#define ARCHON_REWRITE_UNDEF_IR_NODE_REGION
#endif
#ifndef ARCHON_IR_NODE_MERGE
#define ARCHON_IR_NODE_MERGE 4u
#define ARCHON_REWRITE_UNDEF_IR_NODE_MERGE
#endif
#ifndef ARCHON_IR_NODE_PHI
#define ARCHON_IR_NODE_PHI 5u
#define ARCHON_REWRITE_UNDEF_IR_NODE_PHI
#endif
#ifndef ARCHON_IR_NODE_EFFECT_PHI
#define ARCHON_IR_NODE_EFFECT_PHI 6u
#define ARCHON_REWRITE_UNDEF_IR_NODE_EFFECT_PHI
#endif
#ifndef ARCHON_IR_NODE_CONSTANT
#define ARCHON_IR_NODE_CONSTANT 7u
#define ARCHON_REWRITE_UNDEF_IR_NODE_CONSTANT
#endif
#ifndef ARCHON_IR_INPUT_DATA
#define ARCHON_IR_INPUT_DATA 1u
#define ARCHON_REWRITE_UNDEF_IR_INPUT_DATA
#endif
#ifndef ARCHON_IR_INPUT_CONTROL
#define ARCHON_IR_INPUT_CONTROL 2u
#define ARCHON_REWRITE_UNDEF_IR_INPUT_CONTROL
#endif
#ifndef ARCHON_IR_INPUT_EFFECT
#define ARCHON_IR_INPUT_EFFECT 3u
#define ARCHON_REWRITE_UNDEF_IR_INPUT_EFFECT
#endif
#ifndef ARCHON_IR_FLAG_NONE
#define ARCHON_IR_FLAG_NONE 0u
#define ARCHON_REWRITE_UNDEF_IR_FLAG_NONE
#endif
#ifndef ARCHON_IR_FLAG_PURE
#define ARCHON_IR_FLAG_PURE (1u << 0)
#define ARCHON_REWRITE_UNDEF_IR_FLAG_PURE
#endif
#ifndef ARCHON_IR_FLAG_PINNED
#define ARCHON_IR_FLAG_PINNED (1u << 1)
#define ARCHON_REWRITE_UNDEF_IR_FLAG_PINNED
#endif
#ifndef ARCHON_IR_FLAG_MAY_TRAP
#define ARCHON_IR_FLAG_MAY_TRAP (1u << 2)
#define ARCHON_REWRITE_UNDEF_IR_FLAG_MAY_TRAP
#endif
#ifndef ARCHON_IR_FLAG_TERMINATOR
#define ARCHON_IR_FLAG_TERMINATOR (1u << 3)
#define ARCHON_REWRITE_UNDEF_IR_FLAG_TERMINATOR
#endif
#define ARCHON_REWRITE_PROGRAM_STEP(EXPR) do { \
    if (status == ARCHON_REWRITE_OK) \
        status = (EXPR); \
} while (0)
#define ARCHON_IR_PROGRAM_BEGIN(SCHEMA, NODES, INPUTS) \
    ARCHON_REWRITE_PROGRAM_STEP(archon_program_begin( \
        &state, graphs, limits, (SCHEMA), (NODES), (INPUTS)));
#define ARCHON_IR_NODE(ID, KIND, OPCODE, FLAGS, REGION, PAYLOAD, ARITY) \
    ARCHON_REWRITE_PROGRAM_STEP(archon_program_node( \
        &state, (ID), (KIND), (OPCODE), (FLAGS), (REGION), \
        (PAYLOAD), (ARITY), limits));
#define ARCHON_IR_INPUT(NODE, SLOT, KIND, SOURCE) \
    ARCHON_REWRITE_PROGRAM_STEP(archon_program_input( \
        &state, (NODE), (SLOT), (KIND), (SOURCE)));
#define ARCHON_IR_ROOT(ROOT) \
    ARCHON_REWRITE_PROGRAM_STEP(archon_program_root(&state, (ROOT)));
#define ARCHON_IR_PROGRAM_END() \
    ARCHON_REWRITE_PROGRAM_STEP(archon_program_end(&state));
#if defined(ARCHON_REWRITE_PROGRAM_FILE)
#include ARCHON_REWRITE_PROGRAM_FILE
#else
#include "Program.i"
#endif
#undef ARCHON_IR_PROGRAM_END
#undef ARCHON_IR_ROOT
#undef ARCHON_IR_INPUT
#undef ARCHON_IR_NODE
#undef ARCHON_IR_PROGRAM_BEGIN
#undef ARCHON_REWRITE_PROGRAM_STEP
#ifdef ARCHON_REWRITE_UNDEF_IR_FLAG_TERMINATOR
#undef ARCHON_IR_FLAG_TERMINATOR
#undef ARCHON_REWRITE_UNDEF_IR_FLAG_TERMINATOR
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_FLAG_MAY_TRAP
#undef ARCHON_IR_FLAG_MAY_TRAP
#undef ARCHON_REWRITE_UNDEF_IR_FLAG_MAY_TRAP
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_FLAG_PINNED
#undef ARCHON_IR_FLAG_PINNED
#undef ARCHON_REWRITE_UNDEF_IR_FLAG_PINNED
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_FLAG_PURE
#undef ARCHON_IR_FLAG_PURE
#undef ARCHON_REWRITE_UNDEF_IR_FLAG_PURE
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_FLAG_NONE
#undef ARCHON_IR_FLAG_NONE
#undef ARCHON_REWRITE_UNDEF_IR_FLAG_NONE
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_INPUT_EFFECT
#undef ARCHON_IR_INPUT_EFFECT
#undef ARCHON_REWRITE_UNDEF_IR_INPUT_EFFECT
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_INPUT_CONTROL
#undef ARCHON_IR_INPUT_CONTROL
#undef ARCHON_REWRITE_UNDEF_IR_INPUT_CONTROL
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_INPUT_DATA
#undef ARCHON_IR_INPUT_DATA
#undef ARCHON_REWRITE_UNDEF_IR_INPUT_DATA
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_CONSTANT
#undef ARCHON_IR_NODE_CONSTANT
#undef ARCHON_REWRITE_UNDEF_IR_NODE_CONSTANT
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_EFFECT_PHI
#undef ARCHON_IR_NODE_EFFECT_PHI
#undef ARCHON_REWRITE_UNDEF_IR_NODE_EFFECT_PHI
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_PHI
#undef ARCHON_IR_NODE_PHI
#undef ARCHON_REWRITE_UNDEF_IR_NODE_PHI
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_MERGE
#undef ARCHON_IR_NODE_MERGE
#undef ARCHON_REWRITE_UNDEF_IR_NODE_MERGE
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_REGION
#undef ARCHON_IR_NODE_REGION
#undef ARCHON_REWRITE_UNDEF_IR_NODE_REGION
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_END
#undef ARCHON_IR_NODE_END
#undef ARCHON_REWRITE_UNDEF_IR_NODE_END
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_START
#undef ARCHON_IR_NODE_START
#undef ARCHON_REWRITE_UNDEF_IR_NODE_START
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_GENERIC
#undef ARCHON_IR_NODE_GENERIC
#undef ARCHON_REWRITE_UNDEF_IR_NODE_GENERIC
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_NODE_NONE
#undef ARCHON_IR_NODE_NONE
#undef ARCHON_REWRITE_UNDEF_IR_NODE_NONE
#endif
#ifdef ARCHON_REWRITE_UNDEF_IR_PROGRAM_SCHEMA
#undef ARCHON_IR_PROGRAM_SCHEMA
#undef ARCHON_REWRITE_UNDEF_IR_PROGRAM_SCHEMA
#endif
    if (status == ARCHON_REWRITE_OK && !state.ended)
        status = ARCHON_REWRITE_MALFORMED_PROGRAM;
    if (status == ARCHON_REWRITE_OK) {
        status = archon_program_materialize(&state);
    }
    if (status == ARCHON_REWRITE_OK) {
        ArchonGraphsNodeId decoded =
            state.nodes[state.root].mapped;
        status = archon_rewrite_run_generated(
            graphs, decoded, engine, limits, result, stats);
    }
    archon_program_dispose(&state);
    return status;
#else
    (void)graphs;
    (void)engine;
    (void)limits_argument;
    (void)result;
    (void)stats;
    return ARCHON_REWRITE_PROGRAM_UNAVAILABLE;
#endif
}
"""
