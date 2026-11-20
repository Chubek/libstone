"""Generate a bounded, deterministic C99 e-graph instruction selector.

Only a deliberately small, auditable subset of ArchSem is promoted to a
source-IR equality: a pure single-register result whose expression is one
total bit-vector operation over register/immediate leaves.  Every instruction,
including one without semantics, additionally receives an *explicit target*
pattern.  Such a pattern can consume ``ARCHON_MIR_TARGET`` but can never make a
claim about a source operation.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from pathlib import Path
import json
import re

from ..database import _validate, load
from ..diagnostics import ArchonError


NAME = "isel"
ALIASES = ("instruction-selection", "instruction_selector")

_MAX_PATTERN_INPUTS = 4
_SOURCE_OPERATIONS = {
    "+%": "add",
    "-%": "sub",
    "*%": "mul",
    "&": "and",
    "|": "or",
    "^": "xor",
    "<<": "shl",
    "~": "not",
}
_CONVERSIONS = {"zext", "sext", "trunc"}


def _oracle_enabled(context):
    if not isinstance(context, Mapping):
        return True
    options = context.get("options", {})
    if options is None:
        options = {}
    if not isinstance(options, Mapping):
        raise ArchonError("instruction-selection options must be an object")
    enabled = options.get("oracle", True)
    if type(enabled) is not bool:
        raise ArchonError("instruction-selection option 'oracle' must be boolean")
    return enabled


def _member(context, names):
    if isinstance(context, Mapping):
        for name in names:
            if name in context:
                return context[name]
    for name in names:
        if hasattr(context, name):
            return getattr(context, name)
    return None


def _is_database(value):
    return isinstance(value, Mapping) and {
        "version", "architecture", "instructions", "registers",
    } <= value.keys()


def _load_database(value):
    if _is_database(value):
        db = value
    elif isinstance(value, (str, Path)):
        db = load(value)
    elif isinstance(value, tuple) and len(value) == 2:
        return _load_database(value[1])
    else:
        raise ArchonError(f"invalid instruction-selection ArchDB {value!r}")
    _validate(db)
    return db


def _context_databases(context):
    """Normalize the intentionally small context protocol used by generators.

    Current callers expose databases as ``archdbs`` or ``databases``.  The
    extra accepted spellings make the module useful on its own and keep the
    representation plugin independent of the CLI implementation.
    """

    if _is_database(context):
        raw = [context]
    elif isinstance(context, (list, tuple, set, frozenset)):
        raw = context
    else:
        raw = _member(
            context,
            (
                "selected_archdbs", "selected_databases", "architectures",
                "archdbs", "databases", "database", "archdb",
            ),
        )
        if raw is None:
            selected_method = getattr(context, "selected_databases", None)
            raw = selected_method() if callable(selected_method) else None
        elif callable(raw):
            raw = raw()
    if raw is None:
        raise ArchonError("instruction selection requires at least one ArchDB")
    if _is_database(raw) or isinstance(raw, (str, Path)):
        raw = [raw]
    elif isinstance(raw, Mapping):
        raw = list(raw.values())
    elif not isinstance(raw, Iterable):
        raise ArchonError("instruction-selection databases must be an iterable")

    databases = []
    for value in raw:
        db = _load_database(value)
        databases.append(db)

    selected = _member(
        context,
        ("selected_architectures", "isel_architectures", "architecture_names"),
    )
    # A context may use ``architectures`` for the selection, but never confuse
    # a mapping/list of databases with that option.
    if selected is None:
        candidate = _member(context, ("architectures",))
        if candidate is not None and (
            isinstance(candidate, str)
            or (
                isinstance(candidate, Iterable)
                and not isinstance(candidate, Mapping)
                and all(isinstance(item, str) for item in candidate)
            )
        ):
            selected = candidate
    if isinstance(selected, str):
        selected = [selected]
    if selected:
        available = {db["architecture"] for db in databases}
        unknown = sorted(set(selected) - available)
        if unknown:
            raise ArchonError(
                "unknown instruction-selection architecture(s): "
                + ", ".join(unknown)
            )
        wanted = set(selected)
        databases = [
            db for db in databases if db["architecture"] in wanted
        ]
    if not databases:
        raise ArchonError("instruction selection requires at least one selected ArchDB")
    return sorted(
        databases,
        key=lambda db: (
            db["architecture"],
            json.dumps(db, sort_keys=True, separators=(",", ":")),
        ),
    )


def _sanitize_architecture(name):
    sanitized = re.sub(r"[^A-Za-z0-9_]", "_", name)
    sanitized = re.sub(r"_+", "_", sanitized).strip("_") or "Target"
    if sanitized[0].isdigit():
        sanitized = "_" + sanitized
    return sanitized


def _c_string(value):
    result = ['"']
    for byte in value.encode("utf-8"):
        if byte == 34:
            result.append(r"\"")
        elif byte == 92:
            result.append(r"\\")
        elif 32 <= byte < 127 and byte != 63:
            result.append(chr(byte))
        else:
            # Three-digit octal cannot absorb a following hex digit.
            result.append(f"\\{byte:03o}")
    result.append('"')
    return "".join(result)


def _resolve_expression(expression, locals_, active=None):
    if expression["op"] != "local":
        return expression
    name = expression["name"]
    active = set() if active is None else active
    if name in active or name not in locals_:
        return None
    return _resolve_expression(locals_[name], locals_, active | {name})


def _leaf(expression, locals_, inputs):
    expression = _resolve_expression(expression, locals_)
    if expression is None:
        return None
    conversion = None
    while expression["op"] in _CONVERSIONS:
        conversion = expression["op"]
        expression = _resolve_expression(expression["value"], locals_)
        if expression is None:
            return None
    op = expression["op"]
    if op == "read" and conversion is None:
        name = expression["name"]
        if name not in inputs or inputs[name]["kind"] != "reg":
            return None
        return {
            "name": name,
            "constant": False,
            "signed": False,
            "width": int(expression["type"]["width"]),
        }
    if op == "immediate":
        name = expression["name"]
        operand = inputs.get(name)
        if operand is None or operand["kind"] != "imm":
            return None
        # A conversion does not relax the encoded immediate's range.
        return {
            "name": name,
            "constant": True,
            "signed": bool(operand["signed"]),
            "width": int(operand["width"]),
        }
    return None


def _semantic_pattern(instruction):
    semantics = instruction.get("semantics")
    if semantics is None:
        return None
    effects = semantics.get("effects", {})
    if any(effects.get(key) for key in (
        "state_reads", "state_writes", "stack_reads", "stack_writes",
        "memory_reads", "memory_writes",
    )):
        return None

    locals_ = {}
    writes = []
    for statement in semantics["statements"]:
        if statement["op"] == "let":
            locals_[statement["name"]] = statement["value"]
        elif statement["op"] == "write":
            writes.append(statement)
        else:
            return None
    if len(writes) != 1:
        return None

    operands = {operand["name"]: operand for operand in instruction["operands"]}
    output = operands.get(writes[0]["name"])
    if output is None or output["kind"] != "reg" or output["direction"] not in {
        "out", "inout",
    }:
        return None
    # Multiple architectural defs need a tuple/multi-result source operation,
    # which this MachineIR contract intentionally does not guess.
    if sum(
        operand["kind"] == "reg" and operand["direction"] in {"out", "inout"}
        for operand in instruction["operands"]
    ) != 1:
        return None

    expression = _resolve_expression(writes[0]["value"], locals_)
    if expression is None:
        return None
    operation = expression["op"]
    if operation in _SOURCE_OPERATIONS or operation == ">>":
        if operation == ">>":
            left = _resolve_expression(expression["left"], locals_)
            if left is None:
                return None
            source = (
                "ashr"
                if left.get("type", {}).get("kind") == "sint"
                else "lshr"
            )
        else:
            source = _SOURCE_OPERATIONS[operation]
        children = (
            [expression["value"]]
            if operation == "~"
            else [expression["left"], expression["right"]]
        )
    else:
        source = "copy"
        children = [expression]
    leaves = [_leaf(child, locals_, operands) for child in children]
    if any(leaf is None for leaf in leaves) or len(leaves) > _MAX_PATTERN_INPUTS:
        return None

    names = [leaf["name"] for leaf in leaves]
    expected_inputs = [
        operand["name"]
        for operand in instruction["operands"]
        if operand["name"] != output["name"]
        and (
            operand["kind"] == "imm"
            or operand["direction"] in {"in", "inout"}
        )
    ]
    if names != expected_inputs:
        return None
    if sorted(effects.get("reads", [])) != sorted(
        leaf["name"] for leaf in leaves if not leaf["constant"]
    ):
        return None
    if effects.get("writes", []) != [output["name"]]:
        return None
    return {
        "source": source,
        "width": expression["type"]["width"],
        "leaves": leaves,
        "explicit": False,
    }


def _explicit_pattern(instruction, db):
    outputs = {
        operand["name"]
        for operand in instruction["operands"]
        if operand["kind"] == "reg" and operand["direction"] in {"out", "inout"}
    }
    leaves = []
    register_width = (
        db["registers"]["width"]
        if db["registers"] is not None
        else db["address_width"]
    )
    for operand in instruction["operands"]:
        if operand["name"] in outputs and operand["direction"] == "out":
            continue
        if operand["kind"] == "reg":
            leaves.append({
                "name": operand["name"],
                "constant": False,
                "signed": False,
                "width": register_width,
            })
        else:
            leaves.append({
                "name": operand["name"],
                "constant": True,
                "signed": bool(operand["signed"]),
                "width": int(operand["width"]),
            })
    if len(leaves) > _MAX_PATTERN_INPUTS:
        return None
    word_width = (
        instruction["semantics"]["word_width"]
        if instruction.get("semantics") is not None
        else (
            db["registers"]["width"]
            if db["registers"] is not None
            else db["address_width"]
        )
    )
    return {
        "source": "target:" + instruction["name"],
        "width": word_width,
        "leaves": leaves,
        "explicit": True,
    }


def _patterns(db):
    rows = []
    pattern_id = 0
    for instruction in sorted(db["instructions"], key=lambda row: row["name"]):
        patterns = []
        semantic = _semantic_pattern(instruction)
        if semantic is not None:
            patterns.append(semantic)
        explicit = _explicit_pattern(instruction, db)
        if explicit is not None:
            patterns.append(explicit)
        for pattern in patterns:
            leaves = pattern["leaves"]
            const_mask = sum(
                (1 << index) for index, leaf in enumerate(leaves)
                if leaf["constant"]
            )
            signed_mask = sum(
                (1 << index) for index, leaf in enumerate(leaves)
                if leaf["signed"]
            )
            widths = [leaf["width"] for leaf in leaves]
            widths.extend([0] * (_MAX_PATTERN_INPUTS - len(widths)))
            rows.append({
                "id": pattern_id,
                "architecture": db["architecture"],
                "target": instruction["name"],
                "source": pattern["source"],
                "width": pattern["width"],
                "arity": len(leaves),
                "const_mask": const_mask,
                "signed_mask": signed_mask,
                "arg_widths": widths,
                "bytes": instruction["width"] // 8,
                "explicit": pattern["explicit"],
            })
            pattern_id += 1
    return rows


def _pattern_file(db):
    lines = [
        "/* Generated by Archon. This file is an X-macro pattern stream. */",
        "#ifndef ARCHON_ISEL_PATTERN",
        '#error "define ARCHON_ISEL_PATTERN before including this file"',
        "#endif",
        f"ARCHON_ISEL_ARCH_BEGIN({_c_string(db['architecture'])})",
    ]
    for row in _patterns(db):
        widths = ", ".join(f"{width}u" for width in row["arg_widths"])
        lines.append(
            "ARCHON_ISEL_PATTERN("
            f"{_c_string(row['architecture'])}, {row['id']}u, "
            f"{_c_string(row['target'])}, {_c_string(row['source'])}, "
            f"{row['width']}u, {row['arity']}u, {row['const_mask']}u, "
            f"{row['signed_mask']}u, {widths}, {row['bytes']}u, "
            f"{int(row['explicit'])}u)"
        )
    lines.extend((
        f"ARCHON_ISEL_ARCH_END({_c_string(db['architecture'])})",
        "",
    ))
    return "\n".join(lines)


def emit(context):
    """Generate the selector and one target-pattern stream per selected DB."""

    databases = _context_databases(context)
    oracle_is_enabled = _oracle_enabled(context)
    oracle_include = '#include "Oracle.h"\n' if oracle_is_enabled else ""
    oracle_support = _ORACLE_SOURCE if oracle_is_enabled else ""
    oracle_header_api = (
        "/*\n"
        " * Consume a loader-validated ISEL sidecar without taking ownership.\n"
        " * Recognized keys are isel.pattern.<id>.deny (Boolean) and\n"
        " * isel.pattern.<id>.cost (I64); pattern IDs are scoped by the\n"
        " * architecture argument.  Unknown keys are ignored.\n"
        " */\n"
        "struct ArchonOracleAnnotations;\n"
        "enum ArchonISelStatus "
        "archon_isel_select_operation_with_annotations(\n"
        "        const char *architecture, "
        "const ArchonISelOperation *operation,\n"
        "        const struct ArchonOracleAnnotations *annotations,\n"
        "        ArchonISelChoice *choice, char *message,\n"
        "        size_t message_capacity);\n"
        if oracle_is_enabled
        else ""
    )
    standalone_run = (
        "archon_isel_run_with_oracle"
        if oracle_is_enabled
        else "archon_isel_run"
    )
    oracle_output = (
        "status = archon_isel_process_output_with_oracle(\n"
        "            path, status, message, sizeof message);"
        if oracle_is_enabled
        else ""
    )
    seen = {}
    named = []
    for db in databases:
        base = _sanitize_architecture(db["architecture"])
        seen[base] = seen.get(base, 0) + 1
        suffix = "" if seen[base] == 1 else f"_{seen[base]}"
        named.append((db, base + suffix))
    includes = "\n".join(
        f'#include "ISelPattern_{stem}.i"' for _, stem in named
    )
    files = {
        "ISel.h": _HEADER.replace("@ORACLE_API@", oracle_header_api),
        "ISel.c": (
            _SOURCE.replace("@PATTERN_INCLUDES@", includes)
            .replace("@ORACLE_INCLUDE@", oracle_include)
            .replace("@ORACLE_SUPPORT@", oracle_support)
            .replace("@STANDALONE_RUN@", standalone_run)
            .replace("@ORACLE_OUTPUT@", oracle_output)
        ),
    }
    for db, stem in named:
        files[f"ISelPattern_{stem}.i"] = _pattern_file(db)
    return files


_HEADER = r"""/* Generated by Archon: bounded MachineIR instruction selector. */
#ifndef ARCHON_ISEL_H_INCLUDED
#define ARCHON_ISEL_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ArchonISelStatus {
    ARCHON_ISEL_OK = 0,
    ARCHON_ISEL_INVALID_ARGUMENT,
    ARCHON_ISEL_MALFORMED_MIR,
    ARCHON_ISEL_LIMIT_EXCEEDED,
    ARCHON_ISEL_UNKNOWN_ARCHITECTURE,
    ARCHON_ISEL_NO_MATCH,
    ARCHON_ISEL_IO_ERROR,
    ARCHON_ISEL_ORACLE_ERROR
};

typedef enum ArchonISelOperandKind {
    ARCHON_ISEL_OPERAND_VALUE = 0,
    ARCHON_ISEL_OPERAND_CONSTANT,
    ARCHON_ISEL_OPERAND_SYMBOL
} ArchonISelOperandKind;

typedef struct ArchonISelOperand {
    ArchonISelOperandKind kind;
    unsigned width;
    unsigned is_signed;
    uint64_t bits;
} ArchonISelOperand;

typedef struct ArchonISelOperation {
    /*
     * operation names a representation-neutral semantic primitive.  An
     * explicit_target instead requests only that exact target:<instruction>
     * pattern and makes no source-level equivalence claim.
     */
    const char *operation;
    const char *explicit_target;
    unsigned width;
    unsigned effects;
    unsigned definedness;
    const ArchonISelOperand *operands;
    size_t operand_count;
} ArchonISelOperation;

typedef struct ArchonISelChoice {
    const char *target;
    unsigned pattern_id;
    unsigned encoding_bytes;
    unsigned instruction_count;
} ArchonISelChoice;

#define ARCHON_ISEL_MAX_COST_ADJUSTMENT 1048576

typedef enum ArchonISelStatus (*ArchonISelInfluenceQuery)(
    void *context,
    const char *architecture,
    unsigned pattern_id,
    const char *target,
    int *deny,
    int *cost_adjustment,
    char *message,
    size_t message_capacity);

const char *archon_isel_status_string(enum ArchonISelStatus status);
size_t archon_isel_pattern_count(const char *architecture);

/*
 * Select one operation through the generated pattern bank and the same
 * bounded e-graph/cost extractor used by archon_isel_run.  This API is the
 * runtime bridge used by the linked representation-neutral pipeline.
 */
enum ArchonISelStatus archon_isel_select_operation(
        const char *architecture, const ArchonISelOperation *operation,
        ArchonISelChoice *choice, char *message, size_t message_capacity);
enum ArchonISelStatus archon_isel_select_operation_with_influence(
        const char *architecture, const ArchonISelOperation *operation,
        ArchonISelInfluenceQuery influence_query, void *influence_context,
        ArchonISelChoice *choice, char *message, size_t message_capacity);
@ORACLE_API@

/*
 * Compile ISel.c with -DARCHON_MIR_FILE=\"MIR.i\" to embed the MachineIR
 * stream.  Without that definition the runtime remains independently
 * compilable and archon_isel_run reports malformed MIR without output.
 *
 * Decode MIR.i, saturate a bounded e-graph, and write InstrMap.i to output.
 * A null architecture deterministically selects the lexicographically first
 * generated architecture.
 *
 * The primary input is the Scheduler.h MachineIR schema (MIR_SCHEMA,
 * MIR_PROGRAM_BEGIN, MIR_VALUE, MIR_OP, MIR_INPUT_*, MIR_DEPENDENCY, and
 * matching END records).  For small standalone producers the equivalent
 * compact X-macro stream is:
 *   ARCHON_MIR_BEGIN(1)
 *   ARCHON_MIR_VALUE(id, width, effects, definedness)
 *   ARCHON_MIR_CONSTANT(id, width, value, definedness)
 *   ARCHON_MIR_NODE(id, "add", width, effects, definedness)
 *   ARCHON_MIR_TARGET(id, "ADD", width, effects, definedness)
 *   ARCHON_MIR_OPERAND(node_id, value_id)
 *   ARCHON_MIR_ROOT(value_id)
 *   ARCHON_MIR_END()
 *
 * Compact IDs are dense and one-based; operands must already be declared.
 * Effects and definedness are opaque equality keys and are reproduced in the
 * selected stream.  MIR_BEGIN/MIR_NODE/MIR_CONSTANT/MIR_OPERAND/MIR_ROOT/
 * MIR_END are accepted as compact aliases; ARCHON_MIR_VALUE avoids collision
 * with the canonical seven-argument MIR_VALUE record.
 */
enum ArchonISelStatus archon_isel_run(
        FILE *output, const char *architecture, char *message,
        size_t message_capacity);
int archon_isel_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif
"""


_ORACLE_SOURCE = r"""
struct ArchonISelOracleContext {
    ArchonOracleAnnotations annotations;
    int loaded;
};

static void archon_isel_oracle_message(
        char *message, size_t capacity,
        const ArchonOracleDiagnostic *diagnostic,
        ArchonOracleStatus status) {
    const char *text = diagnostic && diagnostic->message[0] != '\0'
        ? diagnostic->message : archon_oracle_status_string(status);
    archon_isel_message(message, capacity, text);
}

static void archon_isel_oracle_options(ArchonOracleOptions *options) {
    const char *socket_path;
    archon_oracle_options_init(options);
    socket_path = getenv("ARCHON_ORACLE_SOCKET");
    if (socket_path && socket_path[0] != '\0')
        options->socket_path = socket_path;
}

static enum ArchonISelStatus archon_isel_oracle_influence(
        void *opaque, const char *architecture, unsigned pattern_id,
        const char *target, int *deny, int *cost_adjustment,
        char *message, size_t message_capacity) {
    const struct ArchonISelOracleContext *context =
        (const struct ArchonISelOracleContext *)opaque;
    ArchonOracleStatus status;
    int64_t adjustment = INT64_C(0);
    char key[96];
    int length;
    (void)architecture;
    (void)target;
    if (!context || !context->loaded || !deny || !cost_adjustment)
        return ARCHON_ISEL_INVALID_ARGUMENT;
    *deny = 0;
    *cost_adjustment = 0;

    length = snprintf(
        key, sizeof key, "isel.pattern.%u.deny", pattern_id);
    if (length < 0 || (size_t)length >= sizeof key)
        return ARCHON_ISEL_LIMIT_EXCEEDED;
    status = archon_oracle_annotations_lookup_bool(
        &context->annotations, key, deny);
    if (status != ARCHON_ORACLE_STATUS_OK &&
            status != ARCHON_ORACLE_STATUS_NOT_FOUND) {
        archon_isel_message(
            message, message_capacity,
            "Oracle isel deny annotation has the wrong type");
        return ARCHON_ISEL_ORACLE_ERROR;
    }

    length = snprintf(
        key, sizeof key, "isel.pattern.%u.cost", pattern_id);
    if (length < 0 || (size_t)length >= sizeof key)
        return ARCHON_ISEL_LIMIT_EXCEEDED;
    status = archon_oracle_annotations_lookup_i64(
        &context->annotations, key, &adjustment);
    if (status != ARCHON_ORACLE_STATUS_OK &&
            status != ARCHON_ORACLE_STATUS_NOT_FOUND) {
        archon_isel_message(
            message, message_capacity,
            "Oracle isel cost annotation has the wrong type");
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    if (adjustment < -ARCHON_ISEL_MAX_INFLUENCE ||
            adjustment > ARCHON_ISEL_MAX_INFLUENCE) {
        archon_isel_message(
            message, message_capacity,
            "Oracle isel cost annotation is outside the bounded range");
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    *cost_adjustment = (int)adjustment;
    return ARCHON_ISEL_OK;
}

enum ArchonISelStatus archon_isel_select_operation_with_annotations(
        const char *architecture, const ArchonISelOperation *operation,
        const struct ArchonOracleAnnotations *annotations,
        ArchonISelChoice *choice, char *message, size_t message_capacity) {
    struct ArchonISelOracleContext context;
    if (!annotations ||
            annotations->protocol_version !=
                ARCHON_ORACLE_PROTOCOL_VERSION ||
            annotations->stage != ARCHON_ORACLE_STAGE_ISEL) {
        archon_isel_message(
            message, message_capacity,
            "validated ISEL annotations are required");
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    memset(&context, 0, sizeof context);
    context.annotations = *annotations;
    context.loaded = 1;
    return archon_isel_select_operation_with_influence(
        architecture, operation, archon_isel_oracle_influence, &context,
        choice, message, message_capacity);
}

#ifdef ARCHON_MIR_FILE
static enum ArchonISelStatus archon_isel_oracle_snapshot(
        const char *path, unsigned char **bytes, size_t *size,
        char *message, size_t message_capacity) {
    FILE *input;
    long length;
    unsigned char *copy;
    *bytes = NULL;
    *size = 0u;
    input = fopen(path, "rb");
    if (!input) {
        archon_isel_message(
            message, message_capacity,
            "cannot snapshot embedded MIR for Oracle");
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    if (fseek(input, 0L, SEEK_END) != 0 ||
            (length = ftell(input)) < 0 ||
            (uint64_t)length > ARCHON_ORACLE_MAX_ARTIFACT_BYTES ||
            fseek(input, 0L, SEEK_SET) != 0) {
        (void)fclose(input);
        archon_isel_message(
            message, message_capacity,
            "embedded MIR exceeds the Oracle snapshot limit");
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    copy = (unsigned char *)malloc(
        length != 0L ? (size_t)length : 1u);
    if (!copy) {
        (void)fclose(input);
        archon_isel_message(
            message, message_capacity,
            "out of memory snapshotting embedded MIR");
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    if (length != 0L &&
            fread(copy, 1u, (size_t)length, input) != (size_t)length) {
        (void)fclose(input);
        free(copy);
        archon_isel_message(
            message, message_capacity,
            "cannot read embedded MIR for Oracle");
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    if (fclose(input) != 0) {
        free(copy);
        archon_isel_message(
            message, message_capacity,
            "cannot close embedded MIR Oracle snapshot");
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    *bytes = copy;
    *size = (size_t)length;
    return ARCHON_ISEL_OK;
}

static enum ArchonISelStatus archon_isel_oracle_prepare(
        const char *path, struct ArchonISelOracleContext *context,
        char *message, size_t message_capacity) {
    ArchonOracleOptions options;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    enum ArchonISelStatus selection_status;
    unsigned char *before = NULL, *after = NULL;
    size_t before_size = 0u, after_size = 0u;
    int active;
    char sidecar[ARCHON_ORACLE_MAX_PATH_BYTES + 1u];
    archon_oracle_annotations_init(&context->annotations);
    context->loaded = 0;
    archon_isel_oracle_options(&options);
    active = options.socket_path && options.socket_path[0] != '\0';
    if (active) {
        selection_status = archon_isel_oracle_snapshot(
            path, &before, &before_size, message, message_capacity);
        if (selection_status != ARCHON_ISEL_OK)
            return selection_status;
    }
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_process_file(
        path, ARCHON_ORACLE_STAGE_ISEL, &options, &diagnostic);
    if (status != ARCHON_ORACLE_STATUS_OK &&
            status != ARCHON_ORACLE_STATUS_INACTIVE) {
        free(before);
        archon_isel_oracle_message(
            message, message_capacity, &diagnostic, status);
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    status = archon_oracle_default_annotation_path(
        path, sidecar, sizeof sidecar);
    if (status != ARCHON_ORACLE_STATUS_OK) {
        free(before);
        archon_isel_oracle_message(
            message, message_capacity, NULL, status);
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    if (active) {
        selection_status = archon_isel_oracle_snapshot(
            path, &after, &after_size, message, message_capacity);
        if (selection_status != ARCHON_ISEL_OK) {
            free(before);
            return selection_status;
        }
        if (before_size != after_size ||
                (before_size &&
                 memcmp(before, after, before_size) != 0)) {
            free(after);
            free(before);
            (void)remove(sidecar);
            archon_isel_message(
                message, message_capacity,
                "Oracle transformed compiled-in MIR; rebuild ISel before use");
            return ARCHON_ISEL_ORACLE_ERROR;
        }
        free(after);
        free(before);
    }
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_annotations_load(
        sidecar, &context->annotations, &diagnostic);
    if (status == ARCHON_ORACLE_STATUS_NOT_FOUND)
        return ARCHON_ISEL_OK;
    if (status != ARCHON_ORACLE_STATUS_OK) {
        archon_isel_oracle_message(
            message, message_capacity, &diagnostic, status);
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    if (context->annotations.protocol_version !=
                ARCHON_ORACLE_PROTOCOL_VERSION ||
            context->annotations.stage != ARCHON_ORACLE_STAGE_ISEL) {
        archon_isel_message(
            message, message_capacity,
            "Oracle annotation sidecar has the wrong version or stage");
        archon_oracle_annotations_dispose(&context->annotations);
        return ARCHON_ISEL_ORACLE_ERROR;
    }
    context->loaded = 1;
    return ARCHON_ISEL_OK;
}
#endif

static enum ArchonISelStatus archon_isel_run_with_oracle(
        FILE *output, const char *architecture, char *message,
        size_t message_capacity) {
    struct ArchonISelOracleContext context;
    enum ArchonISelStatus status;
    memset(&context, 0, sizeof context);
#ifdef ARCHON_MIR_FILE
    status = archon_isel_oracle_prepare(
        ARCHON_MIR_FILE, &context, message, message_capacity);
    if (status != ARCHON_ISEL_OK)
        return status;
#else
    archon_oracle_annotations_init(&context.annotations);
#endif
    status = archon_isel_run_impl(
        output, architecture, message, message_capacity,
        context.loaded ? archon_isel_oracle_influence : NULL,
        context.loaded ? &context : NULL);
    archon_oracle_annotations_dispose(&context.annotations);
    return status;
}

static enum ArchonISelStatus archon_isel_process_output_with_oracle(
        const char *path, enum ArchonISelStatus current,
        char *message, size_t message_capacity) {
    ArchonOracleOptions options;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    if (current != ARCHON_ISEL_OK || !path)
        return current;
    archon_isel_oracle_options(&options);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_process_file(
        path, ARCHON_ORACLE_STAGE_ISEL, &options, &diagnostic);
    if (status == ARCHON_ORACLE_STATUS_OK ||
            status == ARCHON_ORACLE_STATUS_INACTIVE)
        return current;
    archon_isel_oracle_message(
        message, message_capacity, &diagnostic, status);
    return ARCHON_ISEL_ORACLE_ERROR;
}
"""


_SOURCE = r"""/* Generated by Archon: bounded e-graph instruction selector. */
#include "ISel.h"
@ORACLE_INCLUDE@

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARCHON_ISEL_MAX_NODES 4096u
#define ARCHON_ISEL_MAX_INPUTS 8u
#define ARCHON_ISEL_MAX_ROOTS 1024u
#define ARCHON_ISEL_MAX_VALUES 8192u
#define ARCHON_ISEL_MAX_DEPENDENCIES 16384u
#define ARCHON_ISEL_MAX_CANDIDATES 8192u
#define ARCHON_ISEL_ITERATIONS 16u
#define ARCHON_ISEL_OP_SIZE 64u
#define ARCHON_ISEL_INF 0x3fffffffu
#define ARCHON_ISEL_NO_PATTERN UINT32_MAX
#define ARCHON_ISEL_MAX_INFLUENCE ARCHON_ISEL_MAX_COST_ADJUSTMENT

struct ArchonISelPattern {
    const char *architecture;
    unsigned id;
    const char *target;
    const char *source;
    unsigned width, arity, constant_mask, signed_mask;
    unsigned argument_width[4];
    unsigned encoding_bytes;
    unsigned explicit_only;
};

#define ARCHON_ISEL_ARCH_BEGIN(ARCH_) ARCH_,
#define ARCHON_ISEL_ARCH_END(ARCH_)
#define ARCHON_ISEL_PATTERN(...)
static const char *const archon_isel_architectures[] = {
@PATTERN_INCLUDES@
    NULL
};
#undef ARCHON_ISEL_PATTERN
#undef ARCHON_ISEL_ARCH_END
#undef ARCHON_ISEL_ARCH_BEGIN

#define ARCHON_ISEL_ARCH_BEGIN(ARCH_)
#define ARCHON_ISEL_ARCH_END(ARCH_)
#define ARCHON_ISEL_PATTERN(ARCH_, ID_, TARGET_, SOURCE_, WIDTH_, ARITY_, \
        CONST_, SIGNED_, W0_, W1_, W2_, W3_, BYTES_, EXPLICIT_) \
    {ARCH_, ID_, TARGET_, SOURCE_, WIDTH_, ARITY_, CONST_, SIGNED_, \
     {W0_, W1_, W2_, W3_}, BYTES_, EXPLICIT_},
static const struct ArchonISelPattern archon_isel_patterns[] = {
@PATTERN_INCLUDES@
    {NULL, 0u, NULL, NULL, 0u, 0u, 0u, 0u, {0u, 0u, 0u, 0u}, 0u, 0u}
};
#undef ARCHON_ISEL_PATTERN
#undef ARCHON_ISEL_ARCH_END
#undef ARCHON_ISEL_ARCH_BEGIN

static const size_t archon_isel_pattern_total =
    sizeof archon_isel_patterns / sizeof archon_isel_patterns[0] - 1u;

struct ArchonMIRNode {
    unsigned id;
    unsigned operation_id, function, result_value;
    char operation[ARCHON_ISEL_OP_SIZE];
    unsigned width, effects, definedness;
    uint64_t immediate;
    unsigned first_input, input_count;
    unsigned is_constant, is_value, is_operation;
};

struct ArchonMIRValue {
    unsigned function, value, node, width;
    unsigned parameter, defined;
};

struct ArchonMIRDependency {
    unsigned operation, predecessor, flags;
};

struct ArchonMIR {
    struct ArchonMIRNode nodes[ARCHON_ISEL_MAX_NODES];
    unsigned inputs[ARCHON_ISEL_MAX_NODES * ARCHON_ISEL_MAX_INPUTS];
    unsigned roots[ARCHON_ISEL_MAX_ROOTS];
    struct ArchonMIRValue values[ARCHON_ISEL_MAX_VALUES];
    struct ArchonMIRDependency dependencies[ARCHON_ISEL_MAX_DEPENDENCIES];
    unsigned operation_nodes[ARCHON_ISEL_MAX_NODES];
    unsigned node_count, input_count, root_count;
    unsigned value_count, dependency_count, operation_count;
    unsigned began, ended, canonical, program_started, default_width;
    enum ArchonISelStatus status;
};

struct ArchonEGraph {
    unsigned parent[ARCHON_ISEL_MAX_NODES];
    unsigned rank[ARCHON_ISEL_MAX_NODES];
};

struct ArchonCandidate {
    unsigned node, pattern, eclass;
    int cost_adjustment;
};

struct ArchonCost {
    unsigned instructions, bytes, candidate;
    int64_t cost_adjustment;
    int valid;
};

static enum ArchonISelStatus archon_isel_run_impl(
        FILE *output, const char *architecture, char *message,
        size_t message_capacity, ArchonISelInfluenceQuery influence_query,
        void *influence_context);

static void archon_isel_message(
        char *message, size_t capacity, const char *text) {
    if (message && capacity) {
        size_t length = strlen(text);
        if (length >= capacity)
            length = capacity - 1u;
        memcpy(message, text, length);
        message[length] = '\0';
    }
}

@ORACLE_SUPPORT@

static int archon_isel_put_string(FILE *output, const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    if (fputc('"', output) == EOF)
        return 0;
    while (*cursor) {
        unsigned byte = *cursor++;
        if (byte == '"' || byte == '\\') {
            if (fputc('\\', output) == EOF ||
                    fputc((int)byte, output) == EOF)
                return 0;
        } else if (byte >= 0x20u && byte <= 0x7eu && byte != '?') {
            if (fputc((int)byte, output) == EOF)
                return 0;
        } else if (fprintf(output, "\\%03o", byte) < 0) {
            return 0;
        }
    }
    return fputc('"', output) != EOF;
}

const char *archon_isel_status_string(enum ArchonISelStatus status) {
    static const char *const names[] = {
        "ok", "invalid argument", "malformed MIR", "selector limit exceeded",
        "unknown architecture", "no legal instruction selection", "I/O error",
        "Oracle error"
    };
    if ((unsigned)status >= sizeof names / sizeof names[0])
        return "unknown status";
    return names[(unsigned)status];
}

static const char *archon_isel_default_architecture(void) {
    const char *best = NULL;
    size_t i;
    for (i = 0u; archon_isel_architectures[i]; ++i)
        if (!best || strcmp(archon_isel_architectures[i], best) < 0)
            best = archon_isel_architectures[i];
    return best;
}

static int archon_isel_has_architecture(const char *architecture) {
    size_t i;
    for (i = 0u; archon_isel_architectures[i]; ++i)
        if (strcmp(archon_isel_architectures[i], architecture) == 0)
            return 1;
    return 0;
}

size_t archon_isel_pattern_count(const char *architecture) {
    size_t count = 0u, i;
    if (!architecture)
        architecture = archon_isel_default_architecture();
    if (!architecture)
        return 0u;
    for (i = 0u; i < archon_isel_pattern_total; ++i)
        if (strcmp(archon_isel_patterns[i].architecture, architecture) == 0)
            ++count;
    return count;
}

static void archon_mir_fail(
        struct ArchonMIR *mir, enum ArchonISelStatus status) {
    if (mir->status == ARCHON_ISEL_OK)
        mir->status = status;
}

static void archon_mir_canonical_record(struct ArchonMIR *mir) {
    if (!mir->canonical || !mir->program_started || mir->ended)
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
}

static void archon_mir_begin(struct ArchonMIR *mir, unsigned version) {
    if (mir->began ||
            (mir->canonical ?
                (version == 0u || version > 2u) :
                version != 1u))
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
    else
        mir->began = 1u;
}

static unsigned archon_mir_append_node(
        struct ArchonMIR *mir, const char *operation, unsigned width,
        unsigned effects, unsigned definedness, uint64_t immediate,
        unsigned is_constant, unsigned is_value, unsigned is_operation) {
    struct ArchonMIRNode *node;
    size_t length;
    if (!mir->began || mir->ended || !operation || width == 0u) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return UINT32_MAX;
    }
    if (mir->node_count >= ARCHON_ISEL_MAX_NODES) {
        archon_mir_fail(mir, ARCHON_ISEL_LIMIT_EXCEEDED);
        return UINT32_MAX;
    }
    length = strlen(operation);
    if (length == 0u || length >= ARCHON_ISEL_OP_SIZE) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return UINT32_MAX;
    }
    node = &mir->nodes[mir->node_count];
    memset(node, 0, sizeof *node);
    node->id = mir->node_count + 1u;
    node->operation_id = node->id;
    memcpy(node->operation, operation, length + 1u);
    node->width = width;
    node->effects = effects;
    node->definedness = definedness;
    node->immediate = immediate;
    node->first_input = mir->input_count;
    node->is_constant = is_constant;
    node->is_value = is_value;
    node->is_operation = is_operation;
    return mir->node_count++;
}

static void archon_mir_node(
        struct ArchonMIR *mir, unsigned id, const char *operation,
        unsigned width, unsigned effects, unsigned definedness,
        uint64_t immediate, unsigned is_constant, unsigned is_value) {
    unsigned index;
    if (id != mir->node_count + 1u) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    index = archon_mir_append_node(
        mir, operation, width, effects, definedness, immediate, is_constant,
        is_value, !is_constant && !is_value);
    if (index != UINT32_MAX)
        mir->nodes[index].operation_id = id;
}

static void archon_mir_add_input(
        struct ArchonMIR *mir, unsigned node_index, unsigned value_index,
        int require_prior) {
    struct ArchonMIRNode *node;
    if (node_index >= mir->node_count || value_index >= mir->node_count ||
            node_index == value_index ||
            (require_prior && value_index >= node_index)) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    node = &mir->nodes[node_index];
    if (node->first_input + node->input_count != mir->input_count ||
            node->input_count >= ARCHON_ISEL_MAX_INPUTS ||
            node->is_constant || node->is_value) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    mir->inputs[mir->input_count++] = value_index + 1u;
    ++node->input_count;
}

static void archon_mir_operand(
        struct ArchonMIR *mir, unsigned node_id, unsigned value_id) {
    if (!mir->began || mir->ended || node_id == 0u ||
            node_id > mir->node_count || value_id == 0u ||
            value_id >= node_id) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    if (node_id != mir->node_count) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    archon_mir_add_input(mir, node_id - 1u, value_id - 1u, 1);
}

static void archon_mir_root(struct ArchonMIR *mir, unsigned value_id) {
    if (!mir->began || mir->ended || value_id == 0u ||
            value_id > mir->node_count) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    if (mir->root_count >= ARCHON_ISEL_MAX_ROOTS) {
        archon_mir_fail(mir, ARCHON_ISEL_LIMIT_EXCEEDED);
        return;
    }
    mir->roots[mir->root_count++] = value_id;
}

static void archon_mir_end(struct ArchonMIR *mir) {
    if (!mir->began || mir->ended || mir->root_count == 0u)
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
    else
        mir->ended = 1u;
}

static struct ArchonMIRValue *archon_mir_find_value(
        struct ArchonMIR *mir, unsigned function, unsigned value) {
    unsigned i;
    for (i = 0u; i < mir->value_count; ++i)
        if (mir->values[i].function == function &&
                mir->values[i].value == value)
            return &mir->values[i];
    return NULL;
}

static void archon_mir_declare_value(
        struct ArchonMIR *mir, unsigned function, unsigned value,
        unsigned width, unsigned parameter) {
    struct ArchonMIRValue *row;
    unsigned node = UINT32_MAX;
    if (!mir->canonical || !mir->program_started || mir->ended || !width ||
            archon_mir_find_value(mir, function, value)) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    if (mir->value_count >= ARCHON_ISEL_MAX_VALUES) {
        archon_mir_fail(mir, ARCHON_ISEL_LIMIT_EXCEEDED);
        return;
    }
    if (parameter)
        node = archon_mir_append_node(
            mir, "value", width, 0u, 0u, 0u, 0u, 1u, 0u);
    row = &mir->values[mir->value_count++];
    row->function = function;
    row->value = value;
    row->node = node;
    row->width = width;
    row->parameter = !!parameter;
    row->defined = !!parameter;
}

static void archon_mir_operation(
        struct ArchonMIR *mir, unsigned operation, unsigned function,
        unsigned result, const char *opcode, unsigned effects) {
    struct ArchonMIRValue *value = NULL;
    unsigned width, node;
    if (!mir->canonical || !mir->program_started || mir->ended ||
            operation >= ARCHON_ISEL_MAX_NODES ||
            mir->operation_nodes[operation] != UINT32_MAX) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    if (result != UINT32_MAX) {
        value = archon_mir_find_value(mir, function, result);
        if (!value || value->defined) {
            archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
            return;
        }
        width = value->width;
    } else {
        width = mir->default_width ? mir->default_width : 1u;
    }
    node = archon_mir_append_node(
        mir, opcode, width, effects, 0u, 0u, 0u, 0u, 1u);
    if (node == UINT32_MAX)
        return;
    mir->nodes[node].operation_id = operation;
    mir->nodes[node].function = function;
    mir->nodes[node].result_value = result;
    mir->operation_nodes[operation] = node;
    if (operation + 1u > mir->operation_count)
        mir->operation_count = operation + 1u;
    if (value) {
        value->node = node;
        value->defined = 1u;
    }
}

static struct ArchonMIRNode *archon_mir_canonical_node(
        struct ArchonMIR *mir, unsigned operation, unsigned *index) {
    if (!mir->canonical || !mir->program_started || mir->ended ||
            operation >= mir->operation_count ||
            mir->operation_nodes[operation] == UINT32_MAX) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return NULL;
    }
    *index = mir->operation_nodes[operation];
    return &mir->nodes[*index];
}

static void archon_mir_input_value(
        struct ArchonMIR *mir, unsigned operation, unsigned argument,
        unsigned value) {
    struct ArchonMIRNode *node;
    struct ArchonMIRValue *row;
    unsigned node_index;
    node = archon_mir_canonical_node(mir, operation, &node_index);
    if (!node || argument != node->input_count) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    row = archon_mir_find_value(mir, node->function, value);
    if (!row || !row->defined || row->node == UINT32_MAX) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    archon_mir_add_input(mir, node_index, row->node, 0);
}

static void archon_mir_input_constant(
        struct ArchonMIR *mir, unsigned operation, unsigned argument,
        unsigned width, uint64_t value) {
    struct ArchonMIRNode *node;
    unsigned node_index, constant;
    node = archon_mir_canonical_node(mir, operation, &node_index);
    if (!node || argument != node->input_count || !width) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    constant = archon_mir_append_node(
        mir, "constant", width, 0u, 0u, value, 1u, 0u, 0u);
    if (constant != UINT32_MAX)
        archon_mir_add_input(mir, node_index, constant, 0);
}

static void archon_mir_input_symbol(
        struct ArchonMIR *mir, unsigned operation, unsigned argument) {
    struct ArchonMIRNode *node;
    unsigned node_index, symbol;
    node = archon_mir_canonical_node(mir, operation, &node_index);
    if (!node || argument != node->input_count) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    symbol = archon_mir_append_node(
        mir, "symbol", node->width, 0u, 0u, 0u, 0u, 1u, 0u);
    if (symbol != UINT32_MAX)
        archon_mir_add_input(mir, node_index, symbol, 0);
}

static void archon_mir_dependency(
        struct ArchonMIR *mir, unsigned operation, unsigned predecessor,
        unsigned flags) {
    struct ArchonMIRDependency *dependency;
    unsigned ignored;
    if (!archon_mir_canonical_node(mir, operation, &ignored) ||
            !archon_mir_canonical_node(mir, predecessor, &ignored)) {
        return;
    }
    if (mir->dependency_count >= ARCHON_ISEL_MAX_DEPENDENCIES) {
        archon_mir_fail(mir, ARCHON_ISEL_LIMIT_EXCEEDED);
        return;
    }
    dependency = &mir->dependencies[mir->dependency_count++];
    dependency->operation = operation;
    dependency->predecessor = predecessor;
    dependency->flags = flags;
}

static void archon_mir_program_end(struct ArchonMIR *mir) {
    unsigned operation;
    if (!mir->canonical || !mir->began || !mir->program_started ||
            mir->ended) {
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
        return;
    }
    for (operation = 0u; operation < mir->operation_count; ++operation) {
        unsigned node = mir->operation_nodes[operation];
        if (node == UINT32_MAX) {
            archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
            return;
        }
        if (mir->root_count >= ARCHON_ISEL_MAX_ROOTS) {
            archon_mir_fail(mir, ARCHON_ISEL_LIMIT_EXCEEDED);
            return;
        }
        mir->roots[mir->root_count++] = node + 1u;
    }
    mir->ended = 1u;
}

static void archon_mir_initialize(struct ArchonMIR *mir) {
    unsigned initialization;
    memset(mir, 0, sizeof *mir);
    mir->status = ARCHON_ISEL_OK;
    for (initialization = 0u;
            initialization < ARCHON_ISEL_MAX_NODES; ++initialization)
        mir->operation_nodes[initialization] = UINT32_MAX;
}

static enum ArchonISelStatus archon_mir_decode(struct ArchonMIR *mir) {
    archon_mir_initialize(mir);
    /* A particular X-macro stream may omit any optional record kind.  Keep
       strict -Wunused-function builds independent of its contents. */
    (void)archon_mir_begin;
    (void)archon_mir_canonical_record;
    (void)archon_mir_append_node;
    (void)archon_mir_node;
    (void)archon_mir_operand;
    (void)archon_mir_root;
    (void)archon_mir_end;
    (void)archon_mir_declare_value;
    (void)archon_mir_operation;
    (void)archon_mir_input_value;
    (void)archon_mir_input_constant;
    (void)archon_mir_input_symbol;
    (void)archon_mir_dependency;
    (void)archon_mir_program_end;
#define ARCHON_MIR_BEGIN(VERSION_) \
    do { archon_mir_begin(mir, (unsigned)(VERSION_)); } while (0);
#define ARCHON_MIR_VALUE(ID_, WIDTH_, EFFECTS_, DEFINED_) \
    do { archon_mir_node(mir, (ID_), "value", (WIDTH_), (EFFECTS_), \
                         (DEFINED_), 0u, 0u, 1u); } while (0);
#define ARCHON_MIR_CONSTANT(ID_, WIDTH_, VALUE_, DEFINED_) \
    do { archon_mir_node(mir, (ID_), "constant", (WIDTH_), 0u, \
                         (DEFINED_), (uint64_t)(VALUE_), 1u, 0u); } while (0);
#define ARCHON_MIR_NODE(ID_, OP_, WIDTH_, EFFECTS_, DEFINED_) \
    do { archon_mir_node(mir, (ID_), (OP_), (WIDTH_), (EFFECTS_), \
                         (DEFINED_), 0u, 0u, 0u); } while (0);
#define ARCHON_MIR_TARGET(ID_, OP_, WIDTH_, EFFECTS_, DEFINED_) \
    do { archon_mir_node(mir, (ID_), "target:" OP_, (WIDTH_), (EFFECTS_), \
                         (DEFINED_), 0u, 0u, 0u); } while (0);
#define ARCHON_MIR_OPERAND(NODE_, VALUE_) \
    do { archon_mir_operand(mir, (NODE_), (VALUE_)); } while (0);
#define ARCHON_MIR_ROOT(VALUE_) \
    do { archon_mir_root(mir, (VALUE_)); } while (0);
#define ARCHON_MIR_END() do { archon_mir_end(mir); } while (0);
#define ARCHON_ISEL_STRINGIFY_RAW_(VALUE_) #VALUE_
#define ARCHON_ISEL_JOIN_RAW_(LEFT_, RIGHT_) LEFT_ ## RIGHT_
#define ARCHON_ISEL_JOIN_(LEFT_, RIGHT_) \
    ARCHON_ISEL_JOIN_RAW_(LEFT_, RIGHT_)
#define ARCHON_ISEL_ARGUMENT_COUNT_RAW_( \
        A1_, A2_, A3_, A4_, A5_, A6_, A7_, A8_, A9_, A10_, A11_, A12_, \
        A13_, A14_, COUNT_, ...) COUNT_
#define ARCHON_ISEL_ARGUMENT_COUNT_(...) \
    ARCHON_ISEL_ARGUMENT_COUNT_RAW_( \
        __VA_ARGS__, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define ARCHON_ISEL_DISPATCH_RAW_(PREFIX_, COUNT_) \
    ARCHON_ISEL_JOIN_(PREFIX_, COUNT_)
#define ARCHON_ISEL_DISPATCH_(PREFIX_, ...) \
    ARCHON_ISEL_DISPATCH_RAW_( \
        PREFIX_, ARCHON_ISEL_ARGUMENT_COUNT_(__VA_ARGS__))(__VA_ARGS__)
#define MIR_NO_VALUE UINT32_MAX
#define MIR_EFFECT_NONE 0u
#define MIR_EFFECT_READ (1u << 0)
#define MIR_EFFECT_WRITE (1u << 1)
#define MIR_EFFECT_CALL (1u << 2)
#define MIR_EFFECT_CONTROL (1u << 3)
#define MIR_EFFECT_VOLATILE (1u << 4)
#define MIR_EFFECT_TRAP (1u << 5)
#define MIR_EFFECT_IO (1u << 6)
#define MIR_DEP_DATA (1u << 0)
#define MIR_DEP_EFFECT (1u << 1)
#define MIR_DEP_CONTROL (1u << 2)
#define MIR_SCHEMA(VERSION_) do { \
    mir->canonical = 1u; \
    archon_mir_begin(mir, (unsigned)(VERSION_)); \
} while (0);
#define MIR_PROGRAM_BEGIN(NAME_) do { \
    (void)sizeof(NAME_); \
    if (!mir->canonical || !mir->began || mir->program_started || mir->ended) \
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR); \
    else \
        mir->program_started = 1u; \
} while (0);
#define MIR_TARGET(ID_, NAME_, WIDTH_) do { \
    (void)(ID_); (void)sizeof(NAME_); \
    if (!mir->program_started || mir->ended || (unsigned)(WIDTH_) == 0u) \
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR); \
    else if (mir->default_width == 0u) \
        mir->default_width = (unsigned)(WIDTH_); \
} while (0);
#define MIR_TARGET_INSTRUCTION(TARGET_, ORDINAL_, NAME_, ENCODING_BYTES_, \
        ENCODING_KNOWN_, LATENCY_, LATENCY_KNOWN_, THROUGHPUT_, \
        THROUGHPUT_KNOWN_, RESOURCE_, RESOURCE_KNOWN_) do { \
    (void)(TARGET_); (void)(ORDINAL_); (void)sizeof(NAME_); \
    (void)(ENCODING_BYTES_); (void)(ENCODING_KNOWN_); (void)(LATENCY_); \
    (void)(LATENCY_KNOWN_); (void)(THROUGHPUT_); \
    (void)(THROUGHPUT_KNOWN_); (void)sizeof(RESOURCE_); \
    (void)(RESOURCE_KNOWN_); \
    if (!mir->program_started || mir->ended) \
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR); \
} while (0);
#define MIR_FUNCTION_BEGIN(ID_, KEY_, NAME_, TYPE_, WIDTH_) do { \
    (void)(ID_); (void)sizeof(KEY_); (void)sizeof(NAME_); (void)(WIDTH_); \
    if (!mir->program_started || mir->ended) \
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR); \
} while (0);
#define MIR_VALUE(FUNCTION_, ID_, KEY_, NAME_, TYPE_, WIDTH_, PARAMETER_) do { \
    (void)sizeof(KEY_); (void)sizeof(NAME_); \
    archon_mir_declare_value(mir, (FUNCTION_), (ID_), (WIDTH_), (PARAMETER_)); \
} while (0);
#define MIR_BLOCK_BEGIN(FUNCTION_, BLOCK_, KEY_, NAME_) do { \
    (void)(FUNCTION_); (void)(BLOCK_); (void)sizeof(KEY_); (void)sizeof(NAME_); \
    if (!mir->program_started || mir->ended) \
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR); \
} while (0);
#define ARCHON_ISEL_CANDIDATE_SET_BEGIN_3(SET_, OPCODE_, COUNT_) do { \
    archon_mir_canonical_record(mir); \
    (void)(SET_); (void)(COUNT_); \
} while (0);
#define ARCHON_ISEL_CANDIDATE_SET_BEGIN_5( \
        SET_, OPCODE_, COUNT_, COST_, LATENCY_) do { \
    archon_mir_canonical_record(mir); \
    (void)(SET_); (void)(COUNT_); (void)(COST_); (void)(LATENCY_); \
} while (0);
#define MIR_CANDIDATE_SET_BEGIN(...) \
    ARCHON_ISEL_DISPATCH_( \
        ARCHON_ISEL_CANDIDATE_SET_BEGIN_, __VA_ARGS__)
#define ARCHON_ISEL_CANDIDATE_14( \
        SET_, ORDINAL_, TARGET_, TARGET_INSTRUCTION_, \
        TARGET_NAME_, INSTRUCTION_, ENCODING_BYTES_, ENCODING_KNOWN_, \
        LATENCY_, LATENCY_KNOWN_, THROUGHPUT_, THROUGHPUT_KNOWN_, \
        RESOURCE_, RESOURCE_KNOWN_) do { \
    archon_mir_canonical_record(mir); \
    (void)(SET_); (void)(ORDINAL_); (void)(TARGET_); \
    (void)(TARGET_INSTRUCTION_); \
    (void)sizeof(TARGET_NAME_); (void)sizeof(INSTRUCTION_); \
    (void)(ENCODING_BYTES_); (void)(ENCODING_KNOWN_); (void)(LATENCY_); \
    (void)(LATENCY_KNOWN_); (void)(THROUGHPUT_); \
    (void)(THROUGHPUT_KNOWN_); (void)sizeof(RESOURCE_); \
    (void)(RESOURCE_KNOWN_); \
} while (0);
#define ARCHON_ISEL_CANDIDATE_7(SET_, ORDINAL_, TARGET_, TARGET_NAME_, \
        INSTRUCTION_, COST_, LATENCY_) do { \
    archon_mir_canonical_record(mir); \
    (void)(SET_); (void)(ORDINAL_); (void)(TARGET_); \
    (void)sizeof(TARGET_NAME_); (void)sizeof(INSTRUCTION_); \
    (void)(COST_); (void)(LATENCY_); \
} while (0);
#define MIR_CANDIDATE(...) \
    ARCHON_ISEL_DISPATCH_(ARCHON_ISEL_CANDIDATE_, __VA_ARGS__)
#define MIR_CANDIDATE_SET_END(SET_) do { \
    archon_mir_canonical_record(mir); (void)(SET_); \
} while (0);
#define ARCHON_ISEL_MIR_OP_9( \
        ID_, FUNCTION_, BLOCK_, SLOT_, RESULT_, OPCODE_, PRIORITY_, \
        EFFECTS_, CANDIDATES_) do { \
    (void)(BLOCK_); (void)(SLOT_); (void)(PRIORITY_); \
    (void)(CANDIDATES_); \
    archon_mir_operation(mir, (ID_), (FUNCTION_), (unsigned)(RESULT_), \
        ARCHON_ISEL_STRINGIFY_RAW_(OPCODE_), (unsigned)(EFFECTS_)); \
} while (0);
#define ARCHON_ISEL_MIR_OP_10( \
        ID_, FUNCTION_, BLOCK_, SLOT_, RESULT_, OPCODE_, COST_, LATENCY_, \
        EFFECTS_, CANDIDATES_) do { \
    (void)(BLOCK_); (void)(SLOT_); (void)(COST_); (void)(LATENCY_); \
    (void)(CANDIDATES_); \
    archon_mir_operation(mir, (ID_), (FUNCTION_), (unsigned)(RESULT_), \
        ARCHON_ISEL_STRINGIFY_RAW_(OPCODE_), (unsigned)(EFFECTS_)); \
} while (0);
#define MIR_OP(...) \
    ARCHON_ISEL_DISPATCH_(ARCHON_ISEL_MIR_OP_, __VA_ARGS__)
#define MIR_INPUT_VALUE(OP_, INDEX_, VALUE_) do { \
    archon_mir_input_value(mir, (OP_), (INDEX_), (VALUE_)); \
} while (0);
#define MIR_INPUT_INTEGER(OP_, INDEX_, TYPE_, WIDTH_, VALUE_) do { \
    archon_mir_input_constant(mir, (OP_), (INDEX_), (WIDTH_), \
                              (uint64_t)(VALUE_)); \
} while (0);
#define MIR_INPUT_BOOLEAN(OP_, INDEX_, VALUE_) do { \
    archon_mir_input_constant(mir, (OP_), (INDEX_), 1u, \
                              (uint64_t)!!(VALUE_)); \
} while (0);
#define MIR_INPUT_SYMBOL(OP_, INDEX_, SYMBOL_) do { \
    (void)sizeof(SYMBOL_); archon_mir_input_symbol(mir, (OP_), (INDEX_)); \
} while (0);
#define MIR_DEPENDENCY(OP_, PREDECESSOR_, FLAGS_) do { \
    archon_mir_dependency(mir, (OP_), (PREDECESSOR_), (unsigned)(FLAGS_)); \
} while (0);
#define MIR_CALL_TARGET(OP_, TARGET_) do { \
    archon_mir_canonical_record(mir); \
    (void)(OP_); (void)sizeof(TARGET_); \
} while (0);
#define MIR_BRANCH_TARGETS(OP_, YES_, NO_) do { \
    archon_mir_canonical_record(mir); \
    (void)(OP_); (void)sizeof(YES_); (void)sizeof(NO_); \
} while (0);
#define MIR_JUMP_TARGET(OP_, TARGET_) do { \
    archon_mir_canonical_record(mir); \
    (void)(OP_); (void)sizeof(TARGET_); \
} while (0);
#define MIR_BLOCK_END(FUNCTION_, BLOCK_) do { \
    (void)(FUNCTION_); (void)(BLOCK_); \
    if (!mir->program_started || mir->ended) \
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR); \
} while (0);
#define MIR_FUNCTION_END(FUNCTION_) do { \
    (void)(FUNCTION_); \
    if (!mir->program_started || mir->ended) \
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR); \
} while (0);
#define MIR_PROGRAM_END() do { archon_mir_program_end(mir); } while (0);
#define MIR_BEGIN ARCHON_MIR_BEGIN
#define MIR_CONSTANT ARCHON_MIR_CONSTANT
#define MIR_NODE ARCHON_MIR_NODE
#define MIR_TARGET_NODE ARCHON_MIR_TARGET
#define MIR_OPERAND ARCHON_MIR_OPERAND
#define MIR_ROOT ARCHON_MIR_ROOT
#define MIR_END ARCHON_MIR_END
#ifdef ARCHON_MIR_FILE
#include ARCHON_MIR_FILE
#else
    /*
     * Keep the generated selector independently compilable.  A concrete
     * MachineIR stream is an input artifact, not generated C source; callers
     * opt into embedding one with -DARCHON_MIR_FILE=\"MIR.i\".
     */
    archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
#endif
#undef MIR_END
#undef MIR_ROOT
#undef MIR_OPERAND
#undef MIR_TARGET_NODE
#undef MIR_NODE
#undef MIR_CONSTANT
#undef MIR_BEGIN
#undef MIR_PROGRAM_END
#undef MIR_FUNCTION_END
#undef MIR_BLOCK_END
#undef MIR_JUMP_TARGET
#undef MIR_BRANCH_TARGETS
#undef MIR_CALL_TARGET
#undef MIR_DEPENDENCY
#undef MIR_INPUT_SYMBOL
#undef MIR_INPUT_BOOLEAN
#undef MIR_INPUT_INTEGER
#undef MIR_INPUT_VALUE
#undef MIR_OP
#undef MIR_CANDIDATE_SET_END
#undef MIR_CANDIDATE
#undef MIR_CANDIDATE_SET_BEGIN
#undef MIR_BLOCK_BEGIN
#undef MIR_VALUE
#undef MIR_FUNCTION_BEGIN
#undef MIR_TARGET_INSTRUCTION
#undef MIR_TARGET
#undef MIR_PROGRAM_BEGIN
#undef MIR_SCHEMA
#undef MIR_DEP_CONTROL
#undef MIR_DEP_EFFECT
#undef MIR_DEP_DATA
#undef MIR_EFFECT_IO
#undef MIR_EFFECT_TRAP
#undef MIR_EFFECT_VOLATILE
#undef MIR_EFFECT_CONTROL
#undef MIR_EFFECT_CALL
#undef MIR_EFFECT_WRITE
#undef MIR_EFFECT_READ
#undef MIR_EFFECT_NONE
#undef MIR_NO_VALUE
#undef ARCHON_ISEL_MIR_OP_10
#undef ARCHON_ISEL_MIR_OP_9
#undef ARCHON_ISEL_CANDIDATE_7
#undef ARCHON_ISEL_CANDIDATE_14
#undef ARCHON_ISEL_CANDIDATE_SET_BEGIN_5
#undef ARCHON_ISEL_CANDIDATE_SET_BEGIN_3
#undef ARCHON_ISEL_DISPATCH_
#undef ARCHON_ISEL_DISPATCH_RAW_
#undef ARCHON_ISEL_ARGUMENT_COUNT_
#undef ARCHON_ISEL_ARGUMENT_COUNT_RAW_
#undef ARCHON_ISEL_JOIN_
#undef ARCHON_ISEL_JOIN_RAW_
#undef ARCHON_ISEL_STRINGIFY_RAW_
#undef ARCHON_MIR_END
#undef ARCHON_MIR_ROOT
#undef ARCHON_MIR_OPERAND
#undef ARCHON_MIR_TARGET
#undef ARCHON_MIR_NODE
#undef ARCHON_MIR_CONSTANT
#undef ARCHON_MIR_VALUE
#undef ARCHON_MIR_BEGIN
    if (!mir->began || !mir->ended)
        archon_mir_fail(mir, ARCHON_ISEL_MALFORMED_MIR);
    return mir->status;
}

static unsigned archon_egraph_find(
        struct ArchonEGraph *graph, unsigned value) {
    unsigned root = value;
    while (graph->parent[root] != root)
        root = graph->parent[root];
    while (graph->parent[value] != value) {
        unsigned next = graph->parent[value];
        graph->parent[value] = root;
        value = next;
    }
    return root;
}

static int archon_egraph_union(
        struct ArchonEGraph *graph, unsigned left, unsigned right) {
    left = archon_egraph_find(graph, left);
    right = archon_egraph_find(graph, right);
    if (left == right)
        return 0;
    if (graph->rank[left] < graph->rank[right]) {
        unsigned temporary = left;
        left = right;
        right = temporary;
    }
    graph->parent[right] = left;
    if (graph->rank[left] == graph->rank[right])
        ++graph->rank[left];
    return 1;
}

static const char *archon_canonical_operation(const char *operation) {
    if (strcmp(operation, "+%") == 0 || strcmp(operation, "ir.add.wrap") == 0 ||
            strcmp(operation, "MIR_OPCODE_ADD") == 0)
        return "add";
    if (strcmp(operation, "-%") == 0 || strcmp(operation, "ir.sub.wrap") == 0 ||
            strcmp(operation, "MIR_OPCODE_SUB") == 0)
        return "sub";
    if (strcmp(operation, "*%") == 0 || strcmp(operation, "ir.mul.wrap") == 0 ||
            strcmp(operation, "MIR_OPCODE_MUL") == 0)
        return "mul";
    if (strcmp(operation, "&") == 0 || strcmp(operation, "ir.and") == 0 ||
            strcmp(operation, "MIR_OPCODE_AND") == 0)
        return "and";
    if (strcmp(operation, "|") == 0 || strcmp(operation, "ir.or") == 0 ||
            strcmp(operation, "MIR_OPCODE_OR") == 0)
        return "or";
    if (strcmp(operation, "^") == 0 || strcmp(operation, "ir.xor") == 0 ||
            strcmp(operation, "MIR_OPCODE_XOR") == 0)
        return "xor";
    if (strcmp(operation, "<<") == 0 || strcmp(operation, "ir.shl") == 0 ||
            strcmp(operation, "MIR_OPCODE_SHL") == 0)
        return "shl";
    if (strcmp(operation, ">>") == 0 || strcmp(operation, "ir.lshr") == 0 ||
            strcmp(operation, "MIR_OPCODE_LSHR") == 0)
        return "lshr";
    if (strcmp(operation, "ir.ashr") == 0 ||
            strcmp(operation, "MIR_OPCODE_ASHR") == 0)
        return "ashr";
    if (strcmp(operation, "~") == 0 || strcmp(operation, "ir.not") == 0 ||
            strcmp(operation, "MIR_OPCODE_NOT") == 0)
        return "not";
    if (strcmp(operation, "MIR_OPCODE_COPY") == 0)
        return "copy";
    return operation;
}

static int archon_generic_operation(const char *operation) {
    return strcmp(operation, "MIR_OPCODE_CALL") == 0 ||
           strcmp(operation, "MIR_OPCODE_BRANCH") == 0 ||
           strcmp(operation, "MIR_OPCODE_JUMP") == 0 ||
           strcmp(operation, "MIR_OPCODE_RETURN") == 0 ||
           strcmp(operation, "MIR_OPCODE_FENCE") == 0 ||
           strcmp(operation, "MIR_OPCODE_TRAP") == 0;
}

static int archon_nodes_equal(
        const struct ArchonMIR *mir, struct ArchonEGraph *graph,
        const struct ArchonMIRNode *left, const struct ArchonMIRNode *right) {
    unsigned i;
    if (strcmp(archon_canonical_operation(left->operation),
               archon_canonical_operation(right->operation)) != 0 ||
            left->width != right->width || left->effects != right->effects ||
            left->definedness != right->definedness ||
            left->is_constant != right->is_constant ||
            left->is_value != right->is_value ||
            left->input_count != right->input_count ||
            (left->is_constant && left->immediate != right->immediate))
        return 0;
    if (left->is_value)
        return left->id == right->id;
    for (i = 0u; i < left->input_count; ++i) {
        unsigned a = mir->inputs[left->first_input + i] - 1u;
        unsigned b = mir->inputs[right->first_input + i] - 1u;
        if (archon_egraph_find(graph, a) != archon_egraph_find(graph, b))
            return 0;
    }
    return 1;
}

static void archon_egraph_saturate(
        const struct ArchonMIR *mir, struct ArchonEGraph *graph) {
    unsigned i, iteration;
    memset(graph, 0, sizeof *graph);
    for (i = 0u; i < mir->node_count; ++i)
        graph->parent[i] = i;
    for (iteration = 0u; iteration < ARCHON_ISEL_ITERATIONS; ++iteration) {
        int changed = 0;
        unsigned left, right;
        for (left = 0u; left < mir->node_count; ++left)
            for (right = left + 1u; right < mir->node_count; ++right)
                if (archon_nodes_equal(
                        mir, graph, &mir->nodes[left], &mir->nodes[right]))
                    changed |= archon_egraph_union(graph, left, right);
        if (!changed)
            break;
    }
}

static int archon_constant_fits(
        uint64_t value, unsigned source_width, unsigned encoding_width,
        int is_signed) {
    uint64_t source_mask, encoding_mask, source, extended;
    if (source_width == 0u || source_width > 64u ||
            encoding_width == 0u || encoding_width > 64u)
        return 0;
    source_mask = source_width == 64u ?
        UINT64_MAX : (UINT64_C(1) << source_width) - UINT64_C(1);
    encoding_mask = encoding_width == 64u ?
        UINT64_MAX : (UINT64_C(1) << encoding_width) - UINT64_C(1);
    source = value & source_mask;
    extended = value & encoding_mask;
    if (is_signed && encoding_width < 64u &&
            (extended & (UINT64_C(1) << (encoding_width - 1u))))
        extended |= ~encoding_mask;
    return source == (extended & source_mask);
}

static int archon_pattern_matches(
        const struct ArchonMIR *mir, const struct ArchonMIRNode *node,
        const struct ArchonISelPattern *pattern) {
    unsigned i;
    const char *operation = pattern->explicit_only ?
        node->operation : archon_canonical_operation(node->operation);
    if (strcmp(operation, pattern->source) != 0 ||
            node->width != pattern->width ||
            node->input_count != pattern->arity)
        return 0;
    /* Semantic equalities are pure. Explicit target nodes retain arbitrary
       effects but do not claim equivalence to a source operation. */
    if (!pattern->explicit_only && node->effects != 0u)
        return 0;
    for (i = 0u; i < pattern->arity; ++i) {
        const struct ArchonMIRNode *input =
            &mir->nodes[mir->inputs[node->first_input + i] - 1u];
        if (pattern->constant_mask & (1u << i)) {
            if (!input->is_constant ||
                    !archon_constant_fits(
                        input->immediate, input->width,
                        pattern->argument_width[i],
                        (pattern->signed_mask & (1u << i)) != 0u))
                return 0;
        } else if (input->is_constant ||
                (pattern->argument_width[i] != 0u &&
                 input->width != pattern->argument_width[i])) {
            /* A register operand cannot consume a literal without a separate
               materialization instruction. */
            return 0;
        }
    }
    return 1;
}

static enum ArchonISelStatus archon_candidates(
        const struct ArchonMIR *mir, struct ArchonEGraph *graph,
        const char *architecture, struct ArchonCandidate *candidates,
        unsigned *candidate_count, ArchonISelInfluenceQuery influence_query,
        void *influence_context, char *message, size_t message_capacity) {
    unsigned count = 0u, node_index;
    size_t pattern_index;
    for (node_index = 0u; node_index < mir->node_count; ++node_index) {
        const struct ArchonMIRNode *node = &mir->nodes[node_index];
        unsigned legal_match = 0u;
        if (!node->is_operation)
            continue;
        for (pattern_index = 0u;
                pattern_index < archon_isel_pattern_total; ++pattern_index) {
            const struct ArchonISelPattern *pattern =
                &archon_isel_patterns[pattern_index];
            int deny = 0, adjustment = 0;
            enum ArchonISelStatus influence_status;
            if (strcmp(pattern->architecture, architecture) != 0 ||
                    !archon_pattern_matches(mir, node, pattern))
                continue;
            legal_match = 1u;
            /*
             * Influence is deliberately downstream of every semantic,
             * effect, width, arity, and immediate-range check.  It may remove
             * a legal candidate or adjust its extraction preference; it can
             * never manufacture a candidate.
             */
            if (influence_query) {
                influence_status = influence_query(
                    influence_context, pattern->architecture, pattern->id,
                    pattern->target, &deny, &adjustment,
                    message, message_capacity);
                if (influence_status != ARCHON_ISEL_OK)
                    return influence_status;
                if ((deny != 0 && deny != 1) ||
                        adjustment < -ARCHON_ISEL_MAX_INFLUENCE ||
                        adjustment > ARCHON_ISEL_MAX_INFLUENCE) {
                    archon_isel_message(
                        message, message_capacity,
                        "invalid bounded instruction-selection influence");
                    return ARCHON_ISEL_INVALID_ARGUMENT;
                }
                if (deny)
                    continue;
            }
            if (count >= ARCHON_ISEL_MAX_CANDIDATES)
                return ARCHON_ISEL_LIMIT_EXCEEDED;
            candidates[count].node = node_index;
            candidates[count].pattern = (unsigned)pattern_index;
            candidates[count].eclass = archon_egraph_find(graph, node_index);
            candidates[count].cost_adjustment = adjustment;
            ++count;
        }
        /*
         * A denied legal pattern must not reappear through the conservative
         * opaque-operation fallback.  The fallback exists only when no
         * generated pattern described the operation in the first place.
         */
        if (!legal_match &&
                (node->effects != 0u ||
                 archon_generic_operation(node->operation))) {
            if (count >= ARCHON_ISEL_MAX_CANDIDATES)
                return ARCHON_ISEL_LIMIT_EXCEEDED;
            candidates[count].node = node_index;
            candidates[count].pattern = ARCHON_ISEL_NO_PATTERN;
            candidates[count].eclass = archon_egraph_find(graph, node_index);
            candidates[count].cost_adjustment = 0;
            ++count;
        }
    }
    *candidate_count = count;
    return ARCHON_ISEL_OK;
}

static int archon_cost_better(
        const struct ArchonCost *candidate, const struct ArchonCost *current,
        const struct ArchonCandidate *candidates,
        const struct ArchonMIR *mir) {
    const struct ArchonCandidate *candidate_row, *current_row;
    const char *a, *b;
    unsigned a_id, b_id;
    if (!current->valid)
        return 1;
    if (candidate->instructions != current->instructions)
        return candidate->instructions < current->instructions;
    if (candidate->cost_adjustment != current->cost_adjustment)
        return candidate->cost_adjustment < current->cost_adjustment;
    if (candidate->bytes != current->bytes)
        return candidate->bytes < current->bytes;
    candidate_row = &candidates[candidate->candidate];
    current_row = &candidates[current->candidate];
    a = candidate_row->pattern == ARCHON_ISEL_NO_PATTERN ?
        mir->nodes[candidate_row->node].operation :
        archon_isel_patterns[candidate_row->pattern].target;
    b = current_row->pattern == ARCHON_ISEL_NO_PATTERN ?
        mir->nodes[current_row->node].operation :
        archon_isel_patterns[current_row->pattern].target;
    {
        int order = strcmp(a, b);
        if (order)
            return order < 0;
    }
    if (candidates[candidate->candidate].node !=
            candidates[current->candidate].node)
        return candidates[candidate->candidate].node <
               candidates[current->candidate].node;
    a_id = candidate_row->pattern == ARCHON_ISEL_NO_PATTERN ?
        UINT32_MAX : archon_isel_patterns[candidate_row->pattern].id;
    b_id = current_row->pattern == ARCHON_ISEL_NO_PATTERN ?
        UINT32_MAX : archon_isel_patterns[current_row->pattern].id;
    return a_id < b_id;
}

static enum ArchonISelStatus archon_extract_costs(
        const struct ArchonMIR *mir, struct ArchonEGraph *graph,
        const struct ArchonCandidate *candidates, unsigned candidate_count,
        struct ArchonCost *costs) {
    unsigned i, iteration;
    memset(costs, 0, ARCHON_ISEL_MAX_NODES * sizeof *costs);
    for (i = 0u; i < mir->node_count; ++i) {
        unsigned root = archon_egraph_find(graph, i);
        if (mir->nodes[i].is_value || mir->nodes[i].is_constant) {
            costs[root].valid = 1;
            costs[root].instructions = 0u;
            costs[root].bytes = 0u;
            costs[root].candidate = UINT32_MAX;
        }
    }
    for (iteration = 0u; iteration < ARCHON_ISEL_ITERATIONS; ++iteration) {
        int changed = 0;
        for (i = 0u; i < candidate_count; ++i) {
            const struct ArchonCandidate *candidate = &candidates[i];
            const struct ArchonMIRNode *node = &mir->nodes[candidate->node];
            struct ArchonCost proposed;
            unsigned input;
            proposed.valid = 1;
            proposed.instructions = 1u;
            proposed.bytes = candidate->pattern == ARCHON_ISEL_NO_PATTERN ?
                0u : archon_isel_patterns[candidate->pattern].encoding_bytes;
            proposed.cost_adjustment = candidate->cost_adjustment;
            proposed.candidate = i;
            for (input = 0u; input < node->input_count; ++input) {
                unsigned child = archon_egraph_find(
                    graph, mir->inputs[node->first_input + input] - 1u);
                if (!costs[child].valid) {
                    proposed.valid = 0;
                    break;
                }
                if (costs[child].instructions >
                        ARCHON_ISEL_INF - proposed.instructions ||
                        costs[child].bytes >
                        ARCHON_ISEL_INF - proposed.bytes)
                    return ARCHON_ISEL_LIMIT_EXCEEDED;
                proposed.instructions += costs[child].instructions;
                proposed.bytes += costs[child].bytes;
                if ((costs[child].cost_adjustment > 0 &&
                     proposed.cost_adjustment >
                         INT64_MAX - costs[child].cost_adjustment) ||
                    (costs[child].cost_adjustment < 0 &&
                     proposed.cost_adjustment <
                         INT64_MIN - costs[child].cost_adjustment))
                    return ARCHON_ISEL_LIMIT_EXCEEDED;
                proposed.cost_adjustment += costs[child].cost_adjustment;
            }
            if (proposed.valid &&
                    archon_cost_better(
                        &proposed, &costs[candidate->eclass], candidates,
                        mir)) {
                costs[candidate->eclass] = proposed;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }
    return ARCHON_ISEL_OK;
}

struct ArchonEmission {
    unsigned candidate_serial[ARCHON_ISEL_MAX_CANDIDATES];
    unsigned next_serial;
};

static enum ArchonISelStatus archon_emit_class(
        FILE *output, const struct ArchonMIR *mir, struct ArchonEGraph *graph,
        const struct ArchonCandidate *candidates, const struct ArchonCost *costs,
        struct ArchonEmission *emission, unsigned eclass, unsigned *serial) {
    const struct ArchonCandidate *candidate;
    const struct ArchonMIRNode *node;
    const char *target;
    unsigned index, input;
    eclass = archon_egraph_find(graph, eclass);
    if (!costs[eclass].valid)
        return ARCHON_ISEL_NO_MATCH;
    if (costs[eclass].candidate == UINT32_MAX) {
        *serial = 0u;
        return ARCHON_ISEL_OK;
    }
    index = costs[eclass].candidate;
    if (emission->candidate_serial[index]) {
        *serial = emission->candidate_serial[index];
        return ARCHON_ISEL_OK;
    }
    candidate = &candidates[index];
    node = &mir->nodes[candidate->node];
    target = candidate->pattern == ARCHON_ISEL_NO_PATTERN ?
        node->operation : archon_isel_patterns[candidate->pattern].target;
    for (input = 0u; input < node->input_count; ++input) {
        unsigned child_serial;
        enum ArchonISelStatus status = archon_emit_class(
            output, mir, graph, candidates, costs, emission,
            mir->inputs[node->first_input + input] - 1u, &child_serial);
        if (status != ARCHON_ISEL_OK)
            return status;
    }
    *serial = ++emission->next_serial;
    emission->candidate_serial[index] = *serial;
    if (fprintf(
            output, "ARCHON_INSTR(%uu, %uu, ",
            *serial, node->operation_id) < 0 ||
            !archon_isel_put_string(output, target) ||
            fprintf(
                output, ", %uu, %uu, %uu, %uu, %uu)\n",
                node->input_count, costs[eclass].instructions,
                costs[eclass].bytes, node->effects,
                node->definedness) < 0)
        return ARCHON_ISEL_IO_ERROR;
    for (input = 0u; input < node->input_count; ++input) {
        unsigned value = mir->inputs[node->first_input + input];
        unsigned child = archon_egraph_find(graph, value - 1u);
        unsigned child_serial = 0u;
        if (costs[child].candidate != UINT32_MAX)
            child_serial =
                emission->candidate_serial[costs[child].candidate];
        if (fprintf(
                output, "ARCHON_INSTR_OPERAND(%uu, %uu, %uu)\n",
                *serial, value, child_serial) < 0)
            return ARCHON_ISEL_IO_ERROR;
    }
    return ARCHON_ISEL_OK;
}

static void archon_sort_roots(struct ArchonMIR *mir) {
    unsigned i;
    for (i = 1u; i < mir->root_count; ++i) {
        unsigned value = mir->roots[i], position = i;
        while (position && mir->roots[position - 1u] > value) {
            mir->roots[position] = mir->roots[position - 1u];
            --position;
        }
        mir->roots[position] = value;
    }
}

static const char *archon_candidate_target(
        const struct ArchonMIR *mir,
        const struct ArchonCandidate *candidate) {
    return candidate->pattern == ARCHON_ISEL_NO_PATTERN ?
        mir->nodes[candidate->node].operation :
        archon_isel_patterns[candidate->pattern].target;
}

static enum ArchonISelStatus archon_emit_isched_tables(
        FILE *output, const struct ArchonMIR *mir,
        const struct ArchonCandidate *candidates, unsigned candidate_count) {
    unsigned i, previous;
    if (fputs(
            "ISCHED_RESOURCE(0u, \"conservative-selected\", 1u, 1u)\n",
            output) == EOF)
        return ARCHON_ISEL_IO_ERROR;
    for (i = 0u; i < candidate_count; ++i) {
        const char *target = archon_candidate_target(mir, &candidates[i]);
        int duplicate = 0;
        for (previous = 0u; previous < i; ++previous)
            if (strcmp(
                    target,
                    archon_candidate_target(mir, &candidates[previous])) == 0) {
                duplicate = 1;
                break;
            }
        if (!duplicate) {
            if (fputs("ISCHED_INSTRUCTION(", output) == EOF ||
                    !archon_isel_put_string(output, target) ||
                    fputs(", 0u, 1u, 1u, 1u, 1u)\n", output) == EOF)
                return ARCHON_ISEL_IO_ERROR;
        }
    }
    return ARCHON_ISEL_OK;
}

static enum ArchonISelStatus archon_isel_select_operation_impl(
        const char *architecture, const ArchonISelOperation *operation,
        ArchonISelInfluenceQuery influence_query, void *influence_context,
        ArchonISelChoice *choice, char *message, size_t message_capacity) {
    struct ArchonMIR mir;
    struct ArchonEGraph graph;
    struct ArchonCandidate candidates[ARCHON_ISEL_MAX_CANDIDATES];
    struct ArchonCost costs[ARCHON_ISEL_MAX_NODES];
    enum ArchonISelStatus status = ARCHON_ISEL_OK;
    unsigned candidate_count = 0u;
    unsigned root, eclass, selected, input;
    char explicit_operation[ARCHON_ISEL_OP_SIZE];
    const char *source;
    if (!operation || !choice ||
            (!operation->operation && !operation->explicit_target) ||
            (operation->operation && operation->explicit_target) ||
            operation->width == 0u ||
            operation->operand_count > ARCHON_ISEL_MAX_INPUTS ||
            (operation->operand_count && !operation->operands)) {
        archon_isel_message(
            message, message_capacity, "invalid operation selection input");
        return ARCHON_ISEL_INVALID_ARGUMENT;
    }
    archon_isel_message(message, message_capacity, "");
    memset(choice, 0, sizeof *choice);
    if (!architecture)
        architecture = archon_isel_default_architecture();
    if (!architecture || !archon_isel_has_architecture(architecture)) {
        archon_isel_message(
            message, message_capacity, "unknown target architecture");
        return ARCHON_ISEL_UNKNOWN_ARCHITECTURE;
    }
    if (operation->explicit_target) {
        int length = snprintf(
            explicit_operation, sizeof explicit_operation,
            "target:%s", operation->explicit_target);
        if (length < 0 || (size_t)length >= sizeof explicit_operation) {
            archon_isel_message(
                message, message_capacity,
                "explicit target instruction name is too long");
            return ARCHON_ISEL_INVALID_ARGUMENT;
        }
        source = explicit_operation;
    } else {
        source = operation->operation;
    }
    if (!source || source[0] == '\0' ||
            strlen(source) >= ARCHON_ISEL_OP_SIZE) {
        archon_isel_message(
            message, message_capacity, "operation name is invalid");
        return ARCHON_ISEL_INVALID_ARGUMENT;
    }

    archon_mir_initialize(&mir);
    archon_mir_begin(&mir, 1u);
    for (input = 0u; input < operation->operand_count; ++input) {
        const ArchonISelOperand *operand = &operation->operands[input];
        unsigned node;
        if (operand->width == 0u || operand->width > 64u ||
                operand->is_signed > 1u ||
                operand->kind == ARCHON_ISEL_OPERAND_SYMBOL) {
            status = operand->kind == ARCHON_ISEL_OPERAND_SYMBOL
                ? ARCHON_ISEL_NO_MATCH : ARCHON_ISEL_INVALID_ARGUMENT;
            goto failed;
        }
        if (operand->kind == ARCHON_ISEL_OPERAND_VALUE) {
            node = archon_mir_append_node(
                &mir, "value", operand->width, 0u, 0u,
                operand->bits, 0u, 1u, 0u);
        } else if (operand->kind == ARCHON_ISEL_OPERAND_CONSTANT) {
            node = archon_mir_append_node(
                &mir, "constant", operand->width, 0u, 0u,
                operand->bits, 1u, 0u, 0u);
        } else {
            status = ARCHON_ISEL_INVALID_ARGUMENT;
            goto failed;
        }
        if (node == UINT32_MAX) {
            status = mir.status;
            goto failed;
        }
    }
    root = archon_mir_append_node(
        &mir, source, operation->width, operation->effects,
        operation->definedness, 0u, 0u, 0u, 1u);
    if (root == UINT32_MAX) {
        status = mir.status;
        goto failed;
    }
    mir.nodes[root].operation_id = 0u;
    for (input = 0u; input < operation->operand_count; ++input)
        archon_mir_add_input(&mir, root, input, 1);
    archon_mir_root(&mir, root + 1u);
    archon_mir_end(&mir);
    if (mir.status != ARCHON_ISEL_OK) {
        status = mir.status;
        goto failed;
    }

    archon_egraph_saturate(&mir, &graph);
    status = archon_candidates(
        &mir, &graph, architecture, candidates, &candidate_count,
        influence_query, influence_context, message, message_capacity);
    if (status != ARCHON_ISEL_OK)
        goto failed;
    status = archon_extract_costs(
        &mir, &graph, candidates, candidate_count, costs);
    if (status != ARCHON_ISEL_OK)
        goto failed;
    eclass = archon_egraph_find(&graph, root);
    if (!costs[eclass].valid ||
            costs[eclass].candidate == UINT32_MAX) {
        status = ARCHON_ISEL_NO_MATCH;
        goto failed;
    }
    selected = costs[eclass].candidate;
    if (candidates[selected].pattern == ARCHON_ISEL_NO_PATTERN) {
        status = ARCHON_ISEL_NO_MATCH;
        goto failed;
    }
    choice->target = archon_isel_patterns[
        candidates[selected].pattern].target;
    choice->pattern_id = archon_isel_patterns[
        candidates[selected].pattern].id;
    choice->encoding_bytes = archon_isel_patterns[
        candidates[selected].pattern].encoding_bytes;
    choice->instruction_count = costs[eclass].instructions;
    return ARCHON_ISEL_OK;

failed:
    if (!message || !message_capacity || message[0] == '\0')
        archon_isel_message(
            message, message_capacity, archon_isel_status_string(status));
    return status;
}

enum ArchonISelStatus archon_isel_select_operation(
        const char *architecture, const ArchonISelOperation *operation,
        ArchonISelChoice *choice, char *message, size_t message_capacity) {
    return archon_isel_select_operation_impl(
        architecture, operation, NULL, NULL, choice,
        message, message_capacity);
}

enum ArchonISelStatus archon_isel_select_operation_with_influence(
        const char *architecture, const ArchonISelOperation *operation,
        ArchonISelInfluenceQuery influence_query, void *influence_context,
        ArchonISelChoice *choice, char *message, size_t message_capacity) {
    if (!influence_query) {
        archon_isel_message(
            message, message_capacity, "influence query is required");
        return ARCHON_ISEL_INVALID_ARGUMENT;
    }
    return archon_isel_select_operation_impl(
        architecture, operation, influence_query, influence_context,
        choice, message, message_capacity);
}

static enum ArchonISelStatus archon_isel_run_impl(
        FILE *output, const char *architecture, char *message,
        size_t message_capacity, ArchonISelInfluenceQuery influence_query,
        void *influence_context) {
    struct ArchonMIR mir;
    struct ArchonEGraph graph;
    struct ArchonCandidate candidates[ARCHON_ISEL_MAX_CANDIDATES];
    struct ArchonCost costs[ARCHON_ISEL_MAX_NODES];
    struct ArchonEmission emission;
    enum ArchonISelStatus status;
    unsigned candidate_count = 0u, root;
    if (!output)
        return ARCHON_ISEL_INVALID_ARGUMENT;
    archon_isel_message(message, message_capacity, "");
    if (!architecture)
        architecture = archon_isel_default_architecture();
    if (!architecture || !archon_isel_has_architecture(architecture)) {
        archon_isel_message(
            message, message_capacity, "unknown target architecture");
        return ARCHON_ISEL_UNKNOWN_ARCHITECTURE;
    }
    status = archon_mir_decode(&mir);
    if (status != ARCHON_ISEL_OK)
        goto failed;
    archon_egraph_saturate(&mir, &graph);
    status = archon_candidates(
        &mir, &graph, architecture, candidates, &candidate_count,
        influence_query, influence_context, message, message_capacity);
    if (status != ARCHON_ISEL_OK)
        goto failed;
    status = archon_extract_costs(
        &mir, &graph, candidates, candidate_count, costs);
    if (status != ARCHON_ISEL_OK)
        goto failed;
    for (root = 0u; root < mir.root_count; ++root) {
        unsigned eclass =
            archon_egraph_find(&graph, mir.roots[root] - 1u);
        if (!costs[eclass].valid) {
            status = ARCHON_ISEL_NO_MATCH;
            goto failed;
        }
    }
    memset(&emission, 0, sizeof emission);
    archon_sort_roots(&mir);
    if (fputs("ARCHON_INSTR_MAP_BEGIN(", output) == EOF ||
            !archon_isel_put_string(output, architecture) ||
            fputs(")\n", output) == EOF)
        return ARCHON_ISEL_IO_ERROR;
    status = archon_emit_isched_tables(
        output, &mir, candidates, candidate_count);
    if (status != ARCHON_ISEL_OK)
        return status;
    for (root = 0u; root < mir.root_count; ++root) {
        const struct ArchonMIRNode *root_node =
            &mir.nodes[mir.roots[root] - 1u];
        unsigned serial;
        status = archon_emit_class(
            output, &mir, &graph, candidates, costs, &emission,
            mir.roots[root] - 1u, &serial);
        if (status != ARCHON_ISEL_OK)
            goto failed;
        if (fprintf(
                output, "ARCHON_INSTR_ROOT(%uu, %uu)\n",
                root_node->operation_id, serial) < 0)
            return ARCHON_ISEL_IO_ERROR;
    }
    if (mir.canonical) {
        unsigned operation, dependency;
        for (operation = 0u; operation < mir.operation_count; ++operation) {
            unsigned node = mir.operation_nodes[operation];
            unsigned eclass = archon_egraph_find(&graph, node);
            unsigned selected;
            const char *target;
            if (!costs[eclass].valid ||
                    costs[eclass].candidate == UINT32_MAX) {
                status = ARCHON_ISEL_NO_MATCH;
                goto failed;
            }
            selected = costs[eclass].candidate;
            target = archon_candidate_target(&mir, &candidates[selected]);
            if (fprintf(output, "ISCHED_MIR_OP(%uu, ", operation) < 0 ||
                    !archon_isel_put_string(output, target) ||
                    fputs(", 0, 4294967295u)\n", output) == EOF)
                return ARCHON_ISEL_IO_ERROR;
        }
        for (dependency = 0u;
                dependency < mir.dependency_count; ++dependency) {
            const struct ArchonMIRDependency *row =
                &mir.dependencies[dependency];
            if (fprintf(
                    output,
                    "ISCHED_MIR_DEP(%uu, %uu, 1u, 0u, %uu, "
                    "4294967295u, 0u)\n",
                    row->predecessor, row->operation, row->flags) < 0)
                return ARCHON_ISEL_IO_ERROR;
        }
    }
    if (fputs("ARCHON_INSTR_MAP_END()\n", output) == EOF || ferror(output))
        return ARCHON_ISEL_IO_ERROR;
    return ARCHON_ISEL_OK;

failed:
    if (!message || !message_capacity || message[0] == '\0')
        archon_isel_message(
            message, message_capacity, archon_isel_status_string(status));
    return status;
}

enum ArchonISelStatus archon_isel_run(
        FILE *output, const char *architecture, char *message,
        size_t message_capacity) {
    return archon_isel_run_impl(
        output, architecture, message, message_capacity, NULL, NULL);
}

int archon_isel_main(int argc, char **argv) {
    const char *architecture = NULL, *path = NULL;
    FILE *output = stdout;
    enum ArchonISelStatus status;
    char message[160];
    if (argc > 3) {
        fputs("usage: isel [architecture [InstrMap.i]]\n", stderr);
        return 2;
    }
    if (argc >= 2)
        architecture = argv[1];
    if (argc == 3)
        path = argv[2];
    if (path) {
        output = fopen(path, "w");
        if (!output) {
            fprintf(stderr, "cannot open %s\n", path);
            return 2;
        }
    }
    status = @STANDALONE_RUN@(
        output, architecture, message, sizeof message);
    if (path && fclose(output) != 0 && status == ARCHON_ISEL_OK)
        status = ARCHON_ISEL_IO_ERROR;
    @ORACLE_OUTPUT@
    if (status != ARCHON_ISEL_OK) {
        if (path)
            (void)remove(path);
        fprintf(stderr, "instruction selection failed: %s\n",
                message[0] ? message : archon_isel_status_string(status));
        return 1;
    }
    return 0;
}

#ifndef ARCHON_ISEL_NO_MAIN
int main(int argc, char **argv) {
    return archon_isel_main(argc, argv);
}
#endif
"""
