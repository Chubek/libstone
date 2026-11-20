"""Generate a typed, explicit-memory-SSA IR as standalone C99."""

from __future__ import annotations

from collections.abc import Mapping
import re

from .targetfacts import normalize as normalize_targetfacts


NAME = "memory-ssa"
ALIASES = ("memory_ssa", "mem-ssa", "memssa", "mssa")


def _c_string(value: object) -> str:
    data = str(value).encode("utf-8")
    pieces = ['"']
    for byte in data:
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
        elif 32 <= byte < 127:
            pieces.append(chr(byte))
        else:
            # A fixed-width octal escape cannot absorb following hex digits.
            pieces.append(f"\\{byte:03o}")
    pieces.append('"')
    return "".join(pieces)


def _identifier(value: object) -> str:
    text = re.sub(r"[^A-Za-z0-9_]", "_", str(value)).upper()
    if not text or text[0].isdigit():
        text = "_" + text
    return text


def _metadata(context) -> tuple[list[dict], list[dict], list[dict]]:
    if context is None:
        context = {"architectures": []}
    elif isinstance(context, Mapping) and "architectures" not in context:
        for alias in ("archdbs", "databases"):
            if alias in context:
                raw = context[alias]
                if isinstance(raw, Mapping):
                    raw = list(raw.values())
                context = {"architectures": list(raw)}
                break
    architectures = normalize_targetfacts(context)

    architecture_rows: list[dict] = []
    operation_rows: list[dict] = []
    operand_rows: list[dict] = []
    used_constants: set[str] = set()
    effect_flags = {
        "read-scalar": "IR_EFFECT_READ_SCALAR",
        "write-scalar": "IR_EFFECT_WRITE_SCALAR",
        "read-stack": "IR_EFFECT_READ_STACK",
        "write-stack": "IR_EFFECT_WRITE_STACK",
        "read-memory": "IR_EFFECT_READ_MEMORY",
        "write-memory": "IR_EFFECT_WRITE_MEMORY",
    }
    for architecture_index, target in enumerate(architectures):
        architecture_name = target["name"]
        architecture_rows.append(
            {
                "name": architecture_name,
                "address_width": target["address_width"],
                "register_width": target["register_width"],
            }
        )
        classes = {row["local_id"]: row for row in target["classes"]}
        for instruction in sorted(
            target["instructions"],
            key=lambda row: (row["name"], row["id"]),
        ):
            name = instruction["name"]
            base = f"ARCHON_IR_OP_{_identifier(architecture_name)}_{_identifier(name)}"
            constant = base
            suffix = 2
            while constant in used_constants:
                constant = f"{base}_{suffix}"
                suffix += 1
            used_constants.add(constant)
            first_operand = len(operand_rows)
            input_count = 0
            result_count = 0
            for operand in instruction["operands"]:
                operand_class = classes[operand["class_local_id"]]
                direction = operand_class["direction"]
                if direction in {"in", "inout"}:
                    input_count += 1
                if direction in {"out", "inout"}:
                    result_count += 1
                operand_rows.append(
                    {
                        "name": operand["name"],
                        "kind": operand_class["kind"],
                        "direction": direction,
                        "value_width": operand_class["value_width"],
                        "encoded_width": operand_class["encoding_width"],
                        "signed": operand_class["signed"],
                    }
                )
            semantics_known = instruction["semantics_known"]
            effects_known = instruction["effects_known"]
            flags = {
                effect_flags[row["kind"]]
                for row in instruction["effects"]
                if row["kind"] in effect_flags
            }
            if not effects_known:
                flags.update({"IR_EFFECT_OPAQUE", "IR_EFFECT_SIDE_EFFECT"})
            elif flags:
                # A consumer checking only the legacy aggregate flag must still
                # treat every architectural resource access as effectful.
                flags.add("IR_EFFECT_SIDE_EFFECT")
            ordered_flags = sorted(flags)
            requires_sequence = not effects_known or bool(flags)
            operation_rows.append(
                {
                    "constant": constant,
                    "architecture": architecture_index,
                    "name": name,
                    "first_operand": first_operand,
                    "operand_count": len(operand_rows) - first_operand,
                    "input_count": input_count,
                    "result_count": result_count,
                    "semantics_known": semantics_known,
                    "effects_known": effects_known,
                    "equivalence_known": instruction["equivalence_known"],
                    "requires_sequence": requires_sequence,
                    "effects": (
                        " | ".join(ordered_flags)
                        if ordered_flags else "IR_EFFECT_NONE"
                    ),
                }
            )
    return architecture_rows, operation_rows, operand_rows


def emit_ir(context):
    """Emit deterministic Memory SSA source and target metadata."""
    architectures, operations, operands = _metadata(context)
    enum_rows = [
        f"    {row['constant']} = {index + 1},"
        for index, row in enumerate(operations)
    ]
    architecture_rows = [
        "    {"
        f"{_c_string(row['name'])}, {row['address_width']}u, {row['register_width']}u"
        "},"
        for row in architectures
    ]
    operand_rows = []
    for row in operands:
        kind = "IR_OPERAND_REGISTER" if row["kind"] == "reg" else "IR_OPERAND_IMMEDIATE"
        direction = {
            "out": "IR_OPERAND_OUT",
            "inout": "IR_OPERAND_INOUT",
        }.get(row["direction"], "IR_OPERAND_IN")
        operand_rows.append(
            "    {"
            f"{_c_string(row['name'])}, {kind}, {direction}, "
            f"{row['value_width']}u, {row['encoded_width']}u, "
            f"{1 if row['signed'] else 0}u"
            "},"
        )
    operation_rows = [
        "    {"
        f"{index + 1}u, {row['architecture']}u, {_c_string(row['name'])}, "
        f"{row['first_operand']}u, {row['operand_count']}u, "
        f"{row['input_count']}u, {row['result_count']}u, "
        f"{1 if row['semantics_known'] else 0}u, "
        f"{1 if row['effects_known'] else 0}u, "
        f"{1 if row['equivalence_known'] else 0}u, "
        f"{1 if row['requires_sequence'] else 0}u, {row['effects']}"
        "},"
        for index, row in enumerate(operations)
    ]
    replacements = {
        "@OPERATION_ENUM@": "\n".join(enum_rows),
        "@OPERATION_COUNT@": str(len(operations)),
        "@ARCHITECTURE_COUNT@": str(len(architectures)),
        "@ARCHITECTURE_ROWS@": "\n".join(
            architecture_rows or ['    {"", 0u, 0u},']
        ),
        "@OPERAND_COUNT@": str(len(operands)),
        "@OPERAND_ROWS@": "\n".join(
            operand_rows
            or ['    {"", IR_OPERAND_IMMEDIATE, IR_OPERAND_IN, 0u, 0u, 0u},']
        ),
        "@OPERATION_ROWS@": "\n".join(
            operation_rows
            or [
                '    {0u, 0u, "", 0u, 0u, 0u, 0u, '
                '0u, 0u, 0u, 0u, IR_EFFECT_NONE},'
            ]
        ),
    }

    def substitute(template: str) -> str:
        return re.sub(r"@[A-Z_]+@", lambda match: replacements[match.group()], template)

    return {"IR.h": substitute(_HEADER), "IR.c": substitute(_SOURCE)}


_HEADER = r"""/* Generated by Archon: typed Memory SSA IR. */
#ifndef ARCHON_MEMORY_SSA_IR_H
#define ARCHON_MEMORY_SSA_IR_H

#include "Graphs.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t IR_value_id;
typedef uint32_t IR_block_id;
typedef uint32_t IR_node_id;

#define IR_NO_VALUE UINT32_MAX
#define IR_NO_BLOCK UINT32_MAX
#define IR_NO_NODE UINT32_MAX

enum IR_status {
    IR_OK = 0,
    IR_INVALID_ARGUMENT,
    IR_OUT_OF_MEMORY,
    IR_NOT_FOUND,
    IR_TYPE_MISMATCH,
    IR_MALFORMED,
    IR_BUFFER_TOO_SMALL,
    IR_IO_ERROR,
    IR_UNSUPPORTED
};

enum IR_type_kind {
    IR_TYPE_INVALID = 0,
    IR_TYPE_VOID,
    IR_TYPE_BITS,
    IR_TYPE_UINT,
    IR_TYPE_SINT,
    IR_TYPE_FLOAT,
    IR_TYPE_POINTER,
    IR_TYPE_MEMORY,
    IR_TYPE_CONTROL
};

struct IR_type {
    enum IR_type_kind kind;
    uint32_t width;
    uint32_t address_space;
};

enum IR_effect_flags {
    IR_EFFECT_NONE = 0,
    IR_EFFECT_READ_MEMORY = 1u << 0,
    IR_EFFECT_WRITE_MEMORY = 1u << 1,
    IR_EFFECT_SIDE_EFFECT = 1u << 2,
    IR_EFFECT_MAY_TRAP = 1u << 3,
    IR_EFFECT_READ_SCALAR = 1u << 4,
    IR_EFFECT_WRITE_SCALAR = 1u << 5,
    IR_EFFECT_READ_STACK = 1u << 6,
    IR_EFFECT_WRITE_STACK = 1u << 7,
    IR_EFFECT_OPAQUE = 1u << 8
};

enum IR_operand_kind {
    IR_OPERAND_REGISTER = 1,
    IR_OPERAND_IMMEDIATE = 2
};

enum IR_operand_direction {
    IR_OPERAND_IN = 1,
    IR_OPERAND_OUT = 2,
    IR_OPERAND_INOUT = 3
};

enum IR_operation {
    IR_OPERATION_INVALID = 0,
@OPERATION_ENUM@
    IR_OPERATION_COUNT = @OPERATION_COUNT@ + 1
};

struct IR_architecture_metadata {
    const char *name;
    uint32_t address_width;
    uint32_t register_width;
};

struct IR_operand_metadata {
    const char *name;
    enum IR_operand_kind kind;
    enum IR_operand_direction direction;
    uint32_t value_width;
    uint32_t encoded_width;
    unsigned is_signed;
};

struct IR_operation_metadata {
    enum IR_operation operation;
    size_t architecture;
    const char *name;
    size_t first_operand;
    /* Raw encoded operands, including out-only operands. */
    size_t operand_count;
    /* SSA inputs are in and inout operands; results are out and inout. */
    size_t input_count;
    size_t result_count;
    unsigned semantics_known;
    unsigned effects_known;
    /* ArchSem does not establish equivalence to a source-IR operation. */
    unsigned equivalence_known;
    /* Opaque or architectural-resource effects require token ordering. */
    unsigned requires_sequence;
    unsigned effects;
};

enum IR_node_kind {
    IR_NODE_PARAMETER = 1,
    IR_NODE_MEMORY_PARAMETER,
    IR_NODE_CONSTANT,
    IR_NODE_PHI,
    IR_NODE_MEMORY_PHI,
    IR_NODE_LOAD,
    IR_NODE_STORE,
    IR_NODE_CALL,
    IR_NODE_TARGET,
    IR_NODE_JUMP,
    IR_NODE_BRANCH,
    IR_NODE_RETURN
};

struct IR_phi_input {
    IR_block_id block;
    IR_value_id value;
};

struct IR_value {
    IR_value_id id;
    struct IR_type type;
    IR_node_id producer;
    uint32_t memory_version;
    char *name;
};

struct IR_node {
    IR_node_id id;
    IR_block_id block;
    enum IR_node_kind kind;
    struct IR_type type;
    IR_value_id result;
    IR_value_id memory_input;
    IR_value_id memory_result;
    enum IR_operation operation;
    unsigned effects;
    uint32_t alignment;
    uint64_t immediate;
    char *name;
    IR_value_id *operands;
    size_t operand_count;
    struct IR_phi_input *incoming;
    size_t incoming_count;
    IR_block_id target;
    IR_block_id alternative;
};

struct IR_block {
    IR_block_id id;
    char *name;
    IR_node_id *nodes;
    size_t node_count;
    size_t node_capacity;
};

struct IR_program {
    struct IR_block *blocks;
    size_t block_count;
    size_t block_capacity;
    struct IR_node *nodes;
    size_t node_count;
    size_t node_capacity;
    struct IR_value *values;
    size_t value_count;
    size_t value_capacity;
    uint32_t next_memory_version;
    IR_block_id entry;
    ArchonCFG cfg;
};

struct IR_builder {
    struct IR_program *program;
    IR_block_id block;
};

struct IR_diagnostic {
    enum IR_status status;
    IR_block_id block;
    IR_node_id node;
    char message[256];
};

struct IR_type IR_type_make(
    enum IR_type_kind kind, uint32_t width, uint32_t address_space);
struct IR_type IR_memory_type(void);
int IR_type_equal(struct IR_type left, struct IR_type right);
int IR_type_is_valid(struct IR_type type);
const char *IR_type_kind_name(enum IR_type_kind kind);

size_t IR_architecture_count(void);
const struct IR_architecture_metadata *IR_architecture(size_t index);
size_t IR_operation_count(void);
const struct IR_operation_metadata *IR_operation_metadata(
    enum IR_operation operation);
const struct IR_operand_metadata *IR_operation_operand(
    enum IR_operation operation, size_t index);
const struct IR_operand_metadata *IR_operation_input(
    enum IR_operation operation, size_t index);
const struct IR_operand_metadata *IR_operation_result(
    enum IR_operation operation, size_t index);
enum IR_operation IR_find_operation(
    const char *architecture, const char *name);

enum IR_status IR_program_init(struct IR_program *program);
void IR_program_destroy(struct IR_program *program);
enum IR_status IR_add_block(
    struct IR_program *program, const char *name, IR_block_id *block);
enum IR_status IR_set_entry(struct IR_program *program, IR_block_id block);
const struct IR_block *IR_get_block(
    const struct IR_program *program, IR_block_id block);
const struct IR_node *IR_get_node(
    const struct IR_program *program, IR_node_id node);
const struct IR_value *IR_get_value(
    const struct IR_program *program, IR_value_id value);

enum IR_status IR_builder_init(
    struct IR_builder *builder, struct IR_program *program, IR_block_id block);
enum IR_status IR_builder_position(
    struct IR_builder *builder, IR_block_id block);
enum IR_status IR_build_parameter(
    struct IR_builder *builder, struct IR_type type, const char *name,
    IR_value_id *result);
enum IR_status IR_build_memory_parameter(
    struct IR_builder *builder, const char *name, IR_value_id *result);
enum IR_status IR_build_constant(
    struct IR_builder *builder, struct IR_type type, uint64_t value,
    const char *name, IR_value_id *result);
enum IR_status IR_build_phi(
    struct IR_builder *builder, struct IR_type type,
    const struct IR_phi_input *incoming, size_t incoming_count,
    const char *name, IR_value_id *result);
enum IR_status IR_build_memory_phi(
    struct IR_builder *builder, const struct IR_phi_input *incoming,
    size_t incoming_count, const char *name, IR_value_id *result);
enum IR_status IR_build_load(
    struct IR_builder *builder, struct IR_type type, IR_value_id address,
    IR_value_id memory, uint32_t alignment, const char *name,
    IR_value_id *result);
enum IR_status IR_build_store(
    struct IR_builder *builder, IR_value_id address, IR_value_id value,
    IR_value_id memory, uint32_t alignment, IR_value_id *memory_result);
enum IR_status IR_build_call(
    struct IR_builder *builder, const char *callee,
    const IR_value_id *arguments, size_t argument_count,
    struct IR_type result_type, IR_value_id memory, unsigned effects,
    IR_value_id *result, IR_value_id *memory_result);
/*
 * operands contains only in/inout SSA operands, in declaration order.
 * A single out/inout result is supported; multi-result operations return
 * IR_UNSUPPORTED.  metadata.requires_sequence mandates memory and
 * memory_result, including for opaque semantics and scalar/stack effects.
 */
enum IR_status IR_build_target(
    struct IR_builder *builder, enum IR_operation operation,
    const IR_value_id *operands, size_t operand_count,
    struct IR_type result_type, IR_value_id memory,
    IR_value_id *result, IR_value_id *memory_result);
enum IR_status IR_build_jump(
    struct IR_builder *builder, IR_block_id target);
enum IR_status IR_build_branch(
    struct IR_builder *builder, IR_value_id condition,
    IR_block_id if_true, IR_block_id if_false);
enum IR_status IR_build_return(
    struct IR_builder *builder, IR_value_id value, IR_value_id memory);

enum IR_status IR_verify(
    const struct IR_program *program, struct IR_diagnostic *diagnostic);
enum IR_status IR_print(
    FILE *stream, const struct IR_program *program,
    struct IR_diagnostic *diagnostic);

/*
 * Compile IR.c with -DARCHON_IR_PROGRAM_FILE=\"Program.i\" (the legacy
 * ARCHON_IR_PROGRAM spelling is also accepted) to enable the X-macro decoder
 * in IR_load_program.  Without it, IR_load_program returns an empty, valid
 * program.  Program.i may use ARCHON_IR_*, IR_*, or PROGRAM_* spellings.
 */
enum IR_status IR_load_program(
    struct IR_program *program, struct IR_diagnostic *diagnostic);
const char *IR_status_string(enum IR_status status);

#ifdef __cplusplus
}
#endif

#endif
"""


_SOURCE = r"""/* Generated by Archon: typed Memory SSA IR. */
#include "IR.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const struct IR_architecture_metadata IR_architectures[] = {
@ARCHITECTURE_ROWS@
};

static const struct IR_operand_metadata IR_operands[] = {
@OPERAND_ROWS@
};

static const struct IR_operation_metadata IR_operations[] = {
@OPERATION_ROWS@
};

static const size_t IR_architecture_table_count = @ARCHITECTURE_COUNT@u;
static const size_t IR_operation_table_count = @OPERATION_COUNT@u;
static const size_t IR_operand_table_count = @OPERAND_COUNT@u;

static enum IR_status IR_report(
    struct IR_diagnostic *diagnostic, enum IR_status status,
    IR_block_id block, IR_node_id node, const char *format, ...);
static int IR_address_type(struct IR_type type);
static int IR_alignment_valid(uint32_t alignment);
static int IR_effects_valid(unsigned effects);
static int IR_target_type_matches(
    struct IR_type type, const struct IR_operand_metadata *operand);

struct IR_type IR_type_make(
    enum IR_type_kind kind, uint32_t width, uint32_t address_space) {
    struct IR_type type;
    type.kind = kind;
    type.width = width;
    type.address_space = address_space;
    return type;
}

struct IR_type IR_memory_type(void) {
    return IR_type_make(IR_TYPE_MEMORY, 0u, 0u);
}

int IR_type_equal(struct IR_type left, struct IR_type right) {
    return left.kind == right.kind && left.width == right.width &&
           left.address_space == right.address_space;
}

int IR_type_is_valid(struct IR_type type) {
    switch (type.kind) {
    case IR_TYPE_VOID:
    case IR_TYPE_MEMORY:
    case IR_TYPE_CONTROL:
        return type.width == 0u;
    case IR_TYPE_BITS:
    case IR_TYPE_UINT:
    case IR_TYPE_SINT:
    case IR_TYPE_FLOAT:
    case IR_TYPE_POINTER:
        return type.width != 0u;
    default:
        return 0;
    }
}

const char *IR_type_kind_name(enum IR_type_kind kind) {
    switch (kind) {
    case IR_TYPE_VOID: return "void";
    case IR_TYPE_BITS: return "bits";
    case IR_TYPE_UINT: return "uint";
    case IR_TYPE_SINT: return "sint";
    case IR_TYPE_FLOAT: return "float";
    case IR_TYPE_POINTER: return "pointer";
    case IR_TYPE_MEMORY: return "memory";
    case IR_TYPE_CONTROL: return "control";
    default: return "invalid";
    }
}

size_t IR_architecture_count(void) {
    return IR_architecture_table_count;
}

const struct IR_architecture_metadata *IR_architecture(size_t index) {
    return index < IR_architecture_table_count ? &IR_architectures[index] : NULL;
}

size_t IR_operation_count(void) {
    return IR_operation_table_count;
}

const struct IR_operation_metadata *IR_operation_metadata(
    enum IR_operation operation) {
    size_t index;
    if (operation <= IR_OPERATION_INVALID)
        return NULL;
    index = (size_t)operation - 1u;
    if (index >= IR_operation_table_count ||
        IR_operations[index].operation != operation)
        return NULL;
    return &IR_operations[index];
}

const struct IR_operand_metadata *IR_operation_operand(
    enum IR_operation operation, size_t index) {
    const struct IR_operation_metadata *metadata =
        IR_operation_metadata(operation);
    size_t offset;
    if (metadata == NULL || index >= metadata->operand_count)
        return NULL;
    offset = metadata->first_operand + index;
    return offset < IR_operand_table_count ? &IR_operands[offset] : NULL;
}

static const struct IR_operand_metadata *IR_operation_directional_operand(
    enum IR_operation operation, size_t index, int result) {
    const struct IR_operation_metadata *metadata =
        IR_operation_metadata(operation);
    size_t raw_index;
    if (metadata == NULL)
        return NULL;
    for (raw_index = 0u; raw_index < metadata->operand_count; ++raw_index) {
        const struct IR_operand_metadata *operand =
            IR_operation_operand(operation, raw_index);
        int matches = result
            ? (operand->direction == IR_OPERAND_OUT ||
               operand->direction == IR_OPERAND_INOUT)
            : (operand->direction == IR_OPERAND_IN ||
               operand->direction == IR_OPERAND_INOUT);
        if (matches) {
            if (index == 0u)
                return operand;
            --index;
        }
    }
    return NULL;
}

const struct IR_operand_metadata *IR_operation_input(
    enum IR_operation operation, size_t index) {
    return IR_operation_directional_operand(operation, index, 0);
}

const struct IR_operand_metadata *IR_operation_result(
    enum IR_operation operation, size_t index) {
    return IR_operation_directional_operand(operation, index, 1);
}

enum IR_operation IR_find_operation(
    const char *architecture, const char *name) {
    size_t i;
    if (architecture == NULL || name == NULL)
        return IR_OPERATION_INVALID;
    for (i = 0u; i < IR_operation_table_count; ++i) {
        const struct IR_operation_metadata *operation = &IR_operations[i];
        if (operation->architecture < IR_architecture_table_count &&
            strcmp(IR_architectures[operation->architecture].name, architecture) == 0 &&
            strcmp(operation->name, name) == 0)
            return operation->operation;
    }
    return IR_OPERATION_INVALID;
}

static char *IR_copy_string(const char *text) {
    size_t size;
    char *copy;
    if (text == NULL)
        text = "";
    size = strlen(text) + 1u;
    copy = (char *)malloc(size);
    if (copy != NULL)
        memcpy(copy, text, size);
    return copy;
}

static int IR_grow(
    void **storage, size_t *capacity, size_t needed, size_t element_size) {
    size_t next;
    void *replacement;
    if (needed <= *capacity)
        return 1;
    next = *capacity != 0u ? *capacity : 8u;
    while (next < needed) {
        if (next > SIZE_MAX / 2u) {
            next = needed;
            break;
        }
        next *= 2u;
    }
    if (next > SIZE_MAX / element_size)
        return 0;
    replacement = realloc(*storage, next * element_size);
    if (replacement == NULL)
        return 0;
    *storage = replacement;
    *capacity = next;
    return 1;
}

static void IR_clear_node(struct IR_node *node) {
    if (node == NULL)
        return;
    free(node->name);
    free(node->operands);
    free(node->incoming);
    memset(node, 0, sizeof(*node));
}

enum IR_status IR_program_init(struct IR_program *program) {
    ArchonGraphStatus graph_status;
    if (program == NULL)
        return IR_INVALID_ARGUMENT;
    memset(program, 0, sizeof(*program));
    program->entry = IR_NO_BLOCK;
    graph_status = archon_cfg_init(&program->cfg);
    return graph_status == ARCHON_GRAPH_OK ? IR_OK : IR_INVALID_ARGUMENT;
}

void IR_program_destroy(struct IR_program *program) {
    size_t i;
    if (program == NULL)
        return;
    for (i = 0u; i < program->block_count; ++i) {
        free(program->blocks[i].name);
        free(program->blocks[i].nodes);
    }
    for (i = 0u; i < program->node_count; ++i)
        IR_clear_node(&program->nodes[i]);
    for (i = 0u; i < program->value_count; ++i)
        free(program->values[i].name);
    free(program->blocks);
    free(program->nodes);
    free(program->values);
    archon_graph_dispose(&program->cfg);
    memset(program, 0, sizeof(*program));
    program->entry = IR_NO_BLOCK;
}

enum IR_status IR_add_block(
    struct IR_program *program, const char *name, IR_block_id *block) {
    ArchonGraphNodeId graph_id;
    ArchonGraphStatus graph_status;
    struct IR_block *row;
    char *copy;
    size_t i;
    if (program == NULL || block == NULL)
        return IR_INVALID_ARGUMENT;
    if (program->block_count >= (size_t)UINT32_MAX)
        return IR_OUT_OF_MEMORY;
    if (name == NULL)
        name = "";
    for (i = 0u; i < program->block_count; ++i)
        if (strcmp(program->blocks[i].name, name) == 0)
            return IR_MALFORMED;
    copy = IR_copy_string(name);
    if (copy == NULL)
        return IR_OUT_OF_MEMORY;
    if (!IR_grow(
            (void **)&program->blocks, &program->block_capacity,
            program->block_count + 1u, sizeof(*program->blocks))) {
        free(copy);
        return IR_OUT_OF_MEMORY;
    }
    graph_status = archon_cfg_add_block(&program->cfg, name, &graph_id);
    if (graph_status != ARCHON_GRAPH_OK) {
        free(copy);
        return graph_status == ARCHON_GRAPH_OUT_OF_MEMORY
            ? IR_OUT_OF_MEMORY : IR_MALFORMED;
    }
    if ((size_t)graph_id != program->block_count) {
        free(copy);
        return IR_MALFORMED;
    }
    row = &program->blocks[program->block_count];
    memset(row, 0, sizeof(*row));
    row->id = (IR_block_id)program->block_count;
    row->name = copy;
    *block = row->id;
    ++program->block_count;
    return IR_OK;
}

enum IR_status IR_set_entry(struct IR_program *program, IR_block_id block) {
    ArchonGraphStatus status;
    if (program == NULL)
        return IR_INVALID_ARGUMENT;
    if ((size_t)block >= program->block_count)
        return IR_NOT_FOUND;
    status = archon_graph_set_entry(&program->cfg, block);
    if (status != ARCHON_GRAPH_OK)
        return IR_MALFORMED;
    program->entry = block;
    return IR_OK;
}

const struct IR_block *IR_get_block(
    const struct IR_program *program, IR_block_id block) {
    if (program == NULL || (size_t)block >= program->block_count)
        return NULL;
    return &program->blocks[block];
}

const struct IR_node *IR_get_node(
    const struct IR_program *program, IR_node_id node) {
    if (program == NULL || (size_t)node >= program->node_count)
        return NULL;
    return &program->nodes[node];
}

const struct IR_value *IR_get_value(
    const struct IR_program *program, IR_value_id value) {
    if (program == NULL || (size_t)value >= program->value_count)
        return NULL;
    return &program->values[value];
}

enum IR_status IR_builder_init(
    struct IR_builder *builder, struct IR_program *program, IR_block_id block) {
    if (builder == NULL || program == NULL ||
        (size_t)block >= program->block_count)
        return IR_INVALID_ARGUMENT;
    builder->program = program;
    builder->block = block;
    return IR_OK;
}

enum IR_status IR_builder_position(
    struct IR_builder *builder, IR_block_id block) {
    if (builder == NULL || builder->program == NULL)
        return IR_INVALID_ARGUMENT;
    if ((size_t)block >= builder->program->block_count)
        return IR_NOT_FOUND;
    builder->block = block;
    return IR_OK;
}

static int IR_value_valid(
    const struct IR_program *program, IR_value_id value) {
    return value != IR_NO_VALUE && (size_t)value < program->value_count;
}

static int IR_memory_valid(
    const struct IR_program *program, IR_value_id value) {
    return IR_value_valid(program, value) &&
        program->values[value].type.kind == IR_TYPE_MEMORY;
}

static enum IR_status IR_copy_values(
    const struct IR_program *program, const IR_value_id *source, size_t count,
    IR_value_id **destination) {
    size_t i;
    *destination = NULL;
    if (count != 0u && source == NULL)
        return IR_INVALID_ARGUMENT;
    if (count > SIZE_MAX / sizeof(**destination))
        return IR_OUT_OF_MEMORY;
    for (i = 0u; i < count; ++i)
        if (!IR_value_valid(program, source[i]))
            return IR_NOT_FOUND;
    if (count != 0u) {
        *destination = (IR_value_id *)malloc(count * sizeof(**destination));
        if (*destination == NULL)
            return IR_OUT_OF_MEMORY;
        memcpy(*destination, source, count * sizeof(**destination));
    }
    return IR_OK;
}

static enum IR_status IR_copy_incoming(
    const struct IR_program *program, const struct IR_phi_input *source,
    size_t count, struct IR_phi_input **destination, int memory) {
    size_t i;
    *destination = NULL;
    if (count == 0u || source == NULL)
        return IR_INVALID_ARGUMENT;
    if (count > SIZE_MAX / sizeof(**destination))
        return IR_OUT_OF_MEMORY;
    for (i = 0u; i < count; ++i) {
        if ((size_t)source[i].block >= program->block_count ||
            !IR_value_valid(program, source[i].value) ||
            (memory && !IR_memory_valid(program, source[i].value)))
            return IR_NOT_FOUND;
    }
    *destination =
        (struct IR_phi_input *)malloc(count * sizeof(**destination));
    if (*destination == NULL)
        return IR_OUT_OF_MEMORY;
    memcpy(*destination, source, count * sizeof(**destination));
    return IR_OK;
}

static enum IR_status IR_append_node(
    struct IR_builder *builder, struct IR_node *node,
    int has_result, struct IR_type result_type, const char *result_name,
    int has_memory_result, IR_value_id *result,
    IR_value_id *memory_result) {
    struct IR_program *program;
    struct IR_block *block;
    IR_node_id node_id;
    char *result_copy = NULL;
    char *memory_copy = NULL;
    size_t value_total;
    if (builder == NULL || builder->program == NULL || node == NULL)
        return IR_INVALID_ARGUMENT;
    program = builder->program;
    if ((size_t)builder->block >= program->block_count)
        return IR_NOT_FOUND;
    if ((has_result && result == NULL) ||
        (has_memory_result && memory_result == NULL) ||
        (has_result && (!IR_type_is_valid(result_type) ||
                        result_type.kind == IR_TYPE_VOID ||
                        result_type.kind == IR_TYPE_MEMORY)) ||
        program->node_count >= (size_t)UINT32_MAX)
        return IR_INVALID_ARGUMENT;
    value_total = (size_t)has_result + (size_t)has_memory_result;
    if (value_total > (size_t)UINT32_MAX - program->value_count)
        return IR_OUT_OF_MEMORY;
    if (has_result) {
        result_copy = IR_copy_string(result_name);
        if (result_copy == NULL)
            return IR_OUT_OF_MEMORY;
    }
    if (has_memory_result) {
        memory_copy = IR_copy_string(result_name);
        if (memory_copy == NULL) {
            free(result_copy);
            return IR_OUT_OF_MEMORY;
        }
    }
    block = &program->blocks[builder->block];
    if (!IR_grow(
            (void **)&program->nodes, &program->node_capacity,
            program->node_count + 1u, sizeof(*program->nodes)) ||
        !IR_grow(
            (void **)&block->nodes, &block->node_capacity,
            block->node_count + 1u, sizeof(*block->nodes)) ||
        !IR_grow(
            (void **)&program->values, &program->value_capacity,
            program->value_count + value_total, sizeof(*program->values))) {
        free(result_copy);
        free(memory_copy);
        return IR_OUT_OF_MEMORY;
    }
    node_id = (IR_node_id)program->node_count;
    node->id = node_id;
    node->block = builder->block;
    node->result = IR_NO_VALUE;
    node->memory_result = IR_NO_VALUE;
    if (has_result) {
        struct IR_value *value = &program->values[program->value_count];
        value->id = (IR_value_id)program->value_count;
        value->type = result_type;
        value->producer = node_id;
        value->memory_version = UINT32_MAX;
        value->name = result_copy;
        node->result = value->id;
        *result = value->id;
        ++program->value_count;
    }
    if (has_memory_result) {
        struct IR_value *value = &program->values[program->value_count];
        value->id = (IR_value_id)program->value_count;
        value->type = IR_memory_type();
        value->producer = node_id;
        value->memory_version = program->next_memory_version++;
        value->name = memory_copy;
        node->memory_result = value->id;
        *memory_result = value->id;
        ++program->value_count;
    }
    program->nodes[program->node_count] = *node;
    block->nodes[block->node_count++] = node_id;
    ++program->node_count;
    memset(node, 0, sizeof(*node));
    return IR_OK;
}

static void IR_node_defaults(struct IR_node *node, enum IR_node_kind kind) {
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->type = IR_type_make(IR_TYPE_VOID, 0u, 0u);
    node->result = IR_NO_VALUE;
    node->memory_input = IR_NO_VALUE;
    node->memory_result = IR_NO_VALUE;
    node->operation = IR_OPERATION_INVALID;
    node->target = IR_NO_BLOCK;
    node->alternative = IR_NO_BLOCK;
}

enum IR_status IR_build_parameter(
    struct IR_builder *builder, struct IR_type type, const char *name,
    IR_value_id *result) {
    struct IR_node node;
    enum IR_status status;
    if (!IR_type_is_valid(type) || type.kind == IR_TYPE_VOID ||
        type.kind == IR_TYPE_MEMORY || result == NULL)
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_PARAMETER);
    node.type = type;
    node.name = IR_copy_string(name);
    if (node.name == NULL)
        return IR_OUT_OF_MEMORY;
    status = IR_append_node(
        builder, &node, 1, type, name, 0, result, NULL);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

static int IR_print_quoted(FILE *stream, const char *text) {
    const unsigned char *cursor = (const unsigned char *)(text != NULL ? text : "");
    if (fputc('"', stream) == EOF)
        return 0;
    while (*cursor != 0u) {
        unsigned char ch = *cursor++;
        if (ch == '"' || ch == '\\') {
            if (fputc('\\', stream) == EOF || fputc((int)ch, stream) == EOF)
                return 0;
        } else if (ch == '\n') {
            if (fputs("\\n", stream) < 0)
                return 0;
        } else if (ch == '\r') {
            if (fputs("\\r", stream) < 0)
                return 0;
        } else if (ch == '\t') {
            if (fputs("\\t", stream) < 0)
                return 0;
        } else if (ch < 0x20u || ch >= 0x7fu) {
            if (fprintf(stream, "\\x%02x", (unsigned)ch) < 0)
                return 0;
        } else if (fputc((int)ch, stream) == EOF) {
            return 0;
        }
    }
    return fputc('"', stream) != EOF;
}

static int IR_print_type(FILE *stream, struct IR_type type) {
    if (type.kind == IR_TYPE_VOID || type.kind == IR_TYPE_MEMORY ||
        type.kind == IR_TYPE_CONTROL)
        return fputs(IR_type_kind_name(type.kind), stream) >= 0;
    if (type.kind == IR_TYPE_POINTER)
        return fprintf(
            stream, "pointer<%" PRIu32 ",%" PRIu32 ">",
            type.width, type.address_space) >= 0;
    return fprintf(
        stream, "%s<%" PRIu32 ">",
        IR_type_kind_name(type.kind), type.width) >= 0;
}

static int IR_print_value(
    FILE *stream, const struct IR_program *program, IR_value_id value) {
    if (!IR_value_valid(program, value))
        return fputs("<invalid-value>", stream) >= 0;
    if (program->values[value].type.kind == IR_TYPE_MEMORY)
        return fprintf(
            stream, "%%m%" PRIu32,
            program->values[value].memory_version) >= 0;
    return fprintf(stream, "%%v%" PRIu32, value) >= 0;
}

static int IR_print_definitions(
    FILE *stream, const struct IR_program *program,
    const struct IR_node *node) {
    int any = 0;
    if (node->result != IR_NO_VALUE) {
        if (!IR_print_value(stream, program, node->result))
            return 0;
        any = 1;
    }
    if (node->memory_result != IR_NO_VALUE) {
        if (any && fputs(", ", stream) < 0)
            return 0;
        if (!IR_print_value(stream, program, node->memory_result))
            return 0;
        any = 1;
    }
    return !any || fputs(" = ", stream) >= 0;
}

enum IR_status IR_print(
    FILE *stream, const struct IR_program *program,
    struct IR_diagnostic *diagnostic) {
    enum IR_status status;
    size_t i;
    if (stream == NULL)
        return IR_report(
            diagnostic, IR_INVALID_ARGUMENT, IR_NO_BLOCK, IR_NO_NODE,
            "output stream is null");
    status = IR_verify(program, diagnostic);
    if (status != IR_OK)
        return status;
    if (fputs("memory-ssa {\n", stream) < 0)
        goto io_error;
    for (i = 0u; i < program->block_count; ++i) {
        const struct IR_block *block = &program->blocks[i];
        size_t j;
        if (fprintf(stream, "  block %%%" PRIu32 " ", block->id) < 0 ||
            !IR_print_quoted(stream, block->name) ||
            fputs(i == (size_t)program->entry ? " entry:\n" : ":\n", stream) < 0)
            goto io_error;
        for (j = 0u; j < block->node_count; ++j) {
            const struct IR_node *node =
                &program->nodes[block->nodes[j]];
            size_t k;
            if (fputs("    ", stream) < 0 ||
                !IR_print_definitions(stream, program, node))
                goto io_error;
            switch (node->kind) {
            case IR_NODE_PARAMETER:
                if (fputs("parameter ", stream) < 0 ||
                    !IR_print_type(stream, node->type) ||
                    fputc(' ', stream) == EOF ||
                    !IR_print_quoted(stream, node->name))
                    goto io_error;
                break;
            case IR_NODE_MEMORY_PARAMETER:
                if (fputs("memory.parameter ", stream) < 0 ||
                    !IR_print_quoted(stream, node->name))
                    goto io_error;
                break;
            case IR_NODE_CONSTANT:
                if (fputs("constant ", stream) < 0 ||
                    !IR_print_type(stream, node->type) ||
                    fprintf(stream, " 0x%" PRIx64, node->immediate) < 0)
                    goto io_error;
                break;
            case IR_NODE_PHI:
            case IR_NODE_MEMORY_PHI:
                if (fputs(
                        node->kind == IR_NODE_PHI ? "phi " : "memory.phi ",
                        stream) < 0)
                    goto io_error;
                for (k = 0u; k < node->incoming_count; ++k) {
                    if ((k != 0u && fputs(", ", stream) < 0) ||
                        fprintf(
                            stream, "[%%%" PRIu32 ": ",
                            node->incoming[k].block) < 0 ||
                        !IR_print_value(
                            stream, program, node->incoming[k].value) ||
                        fputc(']', stream) == EOF)
                        goto io_error;
                }
                break;
            case IR_NODE_LOAD:
                if (fputs("load ", stream) < 0 ||
                    !IR_print_type(stream, node->type) ||
                    fputc(' ', stream) == EOF ||
                    !IR_print_value(stream, program, node->operands[0]) ||
                    fputs(", ", stream) < 0 ||
                    !IR_print_value(stream, program, node->memory_input) ||
                    fprintf(stream, " align %" PRIu32, node->alignment) < 0)
                    goto io_error;
                break;
            case IR_NODE_STORE:
                if (fputs("store ", stream) < 0 ||
                    !IR_print_value(stream, program, node->operands[1]) ||
                    fputs(", ", stream) < 0 ||
                    !IR_print_value(stream, program, node->operands[0]) ||
                    fputs(", ", stream) < 0 ||
                    !IR_print_value(stream, program, node->memory_input) ||
                    fprintf(stream, " align %" PRIu32, node->alignment) < 0)
                    goto io_error;
                break;
            case IR_NODE_CALL:
                if (fputs("call ", stream) < 0 ||
                    !IR_print_quoted(stream, node->name) ||
                    fputc('(', stream) == EOF)
                    goto io_error;
                for (k = 0u; k < node->operand_count; ++k) {
                    if ((k != 0u && fputs(", ", stream) < 0) ||
                        !IR_print_value(stream, program, node->operands[k]))
                        goto io_error;
                }
                if (fputc(')', stream) == EOF)
                    goto io_error;
                if (node->memory_input != IR_NO_VALUE &&
                    (fputs(" memory ", stream) < 0 ||
                     !IR_print_value(
                         stream, program, node->memory_input)))
                    goto io_error;
                break;
            case IR_NODE_TARGET: {
                const struct IR_operation_metadata *metadata =
                    IR_operation_metadata(node->operation);
                const struct IR_architecture_metadata *architecture =
                    metadata != NULL
                    ? IR_architecture(metadata->architecture) : NULL;
                if (fputs("target ", stream) < 0 ||
                    fputs(
                        architecture != NULL ? architecture->name : "<invalid>",
                        stream) < 0 ||
                    fputc('.', stream) == EOF ||
                    fputs(metadata != NULL ? metadata->name : "<invalid>", stream) < 0 ||
                    fputc('(', stream) == EOF)
                    goto io_error;
                for (k = 0u; k < node->operand_count; ++k) {
                    if ((k != 0u && fputs(", ", stream) < 0) ||
                        !IR_print_value(stream, program, node->operands[k]))
                        goto io_error;
                }
                if (fputc(')', stream) == EOF)
                    goto io_error;
                if (node->memory_input != IR_NO_VALUE &&
                    (fputs(" memory ", stream) < 0 ||
                     !IR_print_value(
                         stream, program, node->memory_input)))
                    goto io_error;
                break;
            }
            case IR_NODE_JUMP:
                if (fprintf(stream, "jump %%%" PRIu32, node->target) < 0)
                    goto io_error;
                break;
            case IR_NODE_BRANCH:
                if (fputs("branch ", stream) < 0 ||
                    !IR_print_value(stream, program, node->operands[0]) ||
                    fprintf(
                        stream, ", %%%" PRIu32 ", %%%" PRIu32,
                        node->target, node->alternative) < 0)
                    goto io_error;
                break;
            case IR_NODE_RETURN:
                if (fputs("return", stream) < 0)
                    goto io_error;
                if (node->operand_count != 0u &&
                    (fputc(' ', stream) == EOF ||
                     !IR_print_value(stream, program, node->operands[0])))
                    goto io_error;
                if (node->memory_input != IR_NO_VALUE &&
                    (fputs(" memory ", stream) < 0 ||
                     !IR_print_value(
                         stream, program, node->memory_input)))
                    goto io_error;
                break;
            default:
                goto io_error;
            }
            if (fputc('\n', stream) == EOF)
                goto io_error;
        }
    }
    if (fputs("}\n", stream) < 0)
        goto io_error;
    return IR_report(
        diagnostic, IR_OK, IR_NO_BLOCK, IR_NO_NODE, "ok");

io_error:
    return IR_report(
        diagnostic, IR_IO_ERROR, IR_NO_BLOCK, IR_NO_NODE,
        "cannot write deterministic IR");
}

enum IR_status IR_load_program(
    struct IR_program *program, struct IR_diagnostic *diagnostic) {
    enum IR_status status;
    struct IR_builder builder;
    if (program == NULL)
        return IR_report(
            diagnostic, IR_INVALID_ARGUMENT, IR_NO_BLOCK, IR_NO_NODE,
            "program is null");
    status = IR_program_init(program);
    if (status != IR_OK)
        return IR_report(
            diagnostic, status, IR_NO_BLOCK, IR_NO_NODE,
            "cannot initialize program");
    builder.program = program;
    builder.block = IR_NO_BLOCK;
    (void)builder;

#define IR_PROGRAM_STEP(EXPRESSION) \
    do { if (status == IR_OK) status = (EXPRESSION); } while (0)
#define IR_PROGRAM_EXPECT(EXPECTED, ACTUAL) \
    do { \
        if (status == IR_OK && (uint32_t)(ACTUAL) != (uint32_t)(EXPECTED)) \
            status = IR_MALFORMED; \
    } while (0)
#define IR_PROGRAM_TYPE(KIND, WIDTH) \
    IR_type_make((KIND), (uint32_t)(WIDTH), 0u)
#define IR_PROGRAM_POINTER(WIDTH, SPACE) \
    IR_type_make(IR_TYPE_POINTER, (uint32_t)(WIDTH), (uint32_t)(SPACE))
#define IR_DECLARE_BLOCK(ID, NAME) \
    do { \
        IR_block_id ir_block = IR_NO_BLOCK; \
        IR_PROGRAM_STEP(IR_add_block(program, (NAME), &ir_block)); \
        IR_PROGRAM_EXPECT((ID), ir_block); \
    } while (0)
#define IR_BLOCK(ID) \
    IR_PROGRAM_STEP(IR_builder_position(&builder, (IR_block_id)(ID)))
#define IR_NEW_BLOCK(ID, NAME) \
    do { IR_DECLARE_BLOCK((ID), (NAME)); IR_BLOCK((ID)); } while (0)
#define IR_ENTRY(ID) \
    IR_PROGRAM_STEP(IR_set_entry(program, (IR_block_id)(ID)))
#define IR_PARAMETER(DST, KIND, WIDTH, NAME) \
    do { \
        IR_value_id ir_value = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_parameter( \
            &builder, IR_PROGRAM_TYPE((KIND), (WIDTH)), (NAME), &ir_value)); \
        IR_PROGRAM_EXPECT((DST), ir_value); \
    } while (0)
#define IR_POINTER_PARAMETER(DST, WIDTH, SPACE, NAME) \
    do { \
        IR_value_id ir_value = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_parameter( \
            &builder, IR_PROGRAM_POINTER((WIDTH), (SPACE)), \
            (NAME), &ir_value)); \
        IR_PROGRAM_EXPECT((DST), ir_value); \
    } while (0)
#define IR_MEMORY_PARAMETER(DST, NAME) \
    do { \
        IR_value_id ir_value = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_memory_parameter( \
            &builder, (NAME), &ir_value)); \
        IR_PROGRAM_EXPECT((DST), ir_value); \
    } while (0)
#define IR_CONSTANT(DST, KIND, WIDTH, VALUE, NAME) \
    do { \
        IR_value_id ir_value = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_constant( \
            &builder, IR_PROGRAM_TYPE((KIND), (WIDTH)), \
            (uint64_t)(VALUE), (NAME), &ir_value)); \
        IR_PROGRAM_EXPECT((DST), ir_value); \
    } while (0)
#define IR_PHI2(DST, KIND, WIDTH, V0, B0, V1, B1, NAME) \
    do { \
        const struct IR_phi_input ir_inputs[2] = { \
            {(IR_block_id)(B0), (IR_value_id)(V0)}, \
            {(IR_block_id)(B1), (IR_value_id)(V1)} \
        }; \
        IR_value_id ir_value = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_phi( \
            &builder, IR_PROGRAM_TYPE((KIND), (WIDTH)), \
            ir_inputs, 2u, (NAME), &ir_value)); \
        IR_PROGRAM_EXPECT((DST), ir_value); \
    } while (0)
#define IR_MEMORY_PHI2(DST, V0, B0, V1, B1, NAME) \
    do { \
        const struct IR_phi_input ir_inputs[2] = { \
            {(IR_block_id)(B0), (IR_value_id)(V0)}, \
            {(IR_block_id)(B1), (IR_value_id)(V1)} \
        }; \
        IR_value_id ir_value = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_memory_phi( \
            &builder, ir_inputs, 2u, (NAME), &ir_value)); \
        IR_PROGRAM_EXPECT((DST), ir_value); \
    } while (0)
#define IR_LOAD(DST, KIND, WIDTH, ADDRESS, MEMORY, ALIGNMENT, NAME) \
    do { \
        IR_value_id ir_value = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_load( \
            &builder, IR_PROGRAM_TYPE((KIND), (WIDTH)), \
            (IR_value_id)(ADDRESS), (IR_value_id)(MEMORY), \
            (uint32_t)(ALIGNMENT), (NAME), &ir_value)); \
        IR_PROGRAM_EXPECT((DST), ir_value); \
    } while (0)
#define IR_STORE(DST, ADDRESS, VALUE, MEMORY, ALIGNMENT) \
    do { \
        IR_value_id ir_value = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_store( \
            &builder, (IR_value_id)(ADDRESS), (IR_value_id)(VALUE), \
            (IR_value_id)(MEMORY), (uint32_t)(ALIGNMENT), &ir_value)); \
        IR_PROGRAM_EXPECT((DST), ir_value); \
    } while (0)
#define IR_CALL0(DST, MDST, CALLEE, KIND, WIDTH, MEMORY, EFFECTS) \
    do { \
        IR_value_id ir_result = IR_NO_VALUE; \
        IR_value_id ir_memory = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_call( \
            &builder, (CALLEE), NULL, 0u, \
            IR_PROGRAM_TYPE((KIND), (WIDTH)), (IR_value_id)(MEMORY), \
            (unsigned)(EFFECTS), &ir_result, &ir_memory)); \
        IR_PROGRAM_EXPECT((DST), ir_result); \
        IR_PROGRAM_EXPECT((MDST), ir_memory); \
    } while (0)
#define IR_CALL1(DST, MDST, CALLEE, KIND, WIDTH, MEMORY, EFFECTS, A0) \
    do { \
        const IR_value_id ir_arguments[1] = {(IR_value_id)(A0)}; \
        IR_value_id ir_result = IR_NO_VALUE; \
        IR_value_id ir_memory = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_call( \
            &builder, (CALLEE), ir_arguments, 1u, \
            IR_PROGRAM_TYPE((KIND), (WIDTH)), (IR_value_id)(MEMORY), \
            (unsigned)(EFFECTS), &ir_result, &ir_memory)); \
        IR_PROGRAM_EXPECT((DST), ir_result); \
        IR_PROGRAM_EXPECT((MDST), ir_memory); \
    } while (0)
#define IR_CALL2(DST, MDST, CALLEE, KIND, WIDTH, MEMORY, EFFECTS, A0, A1) \
    do { \
        const IR_value_id ir_arguments[2] = { \
            (IR_value_id)(A0), (IR_value_id)(A1) \
        }; \
        IR_value_id ir_result = IR_NO_VALUE; \
        IR_value_id ir_memory = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_call( \
            &builder, (CALLEE), ir_arguments, 2u, \
            IR_PROGRAM_TYPE((KIND), (WIDTH)), (IR_value_id)(MEMORY), \
            (unsigned)(EFFECTS), &ir_result, &ir_memory)); \
        IR_PROGRAM_EXPECT((DST), ir_result); \
        IR_PROGRAM_EXPECT((MDST), ir_memory); \
    } while (0)
#define IR_TARGET(DST, MDST, OPERATION, KIND, WIDTH, MEMORY, COUNT, VALUES) \
    do { \
        IR_value_id ir_result = IR_NO_VALUE; \
        IR_value_id ir_memory = IR_NO_VALUE; \
        IR_PROGRAM_STEP(IR_build_target( \
            &builder, (OPERATION), (VALUES), (size_t)(COUNT), \
            IR_PROGRAM_TYPE((KIND), (WIDTH)), (IR_value_id)(MEMORY), \
            &ir_result, &ir_memory)); \
        IR_PROGRAM_EXPECT((DST), ir_result); \
        if ((MDST) != IR_NO_VALUE) IR_PROGRAM_EXPECT((MDST), ir_memory); \
    } while (0)
#define IR_JUMP(TARGET) \
    IR_PROGRAM_STEP(IR_build_jump(&builder, (IR_block_id)(TARGET)))
#define IR_BRANCH(CONDITION, YES, NO) \
    IR_PROGRAM_STEP(IR_build_branch( \
        &builder, (IR_value_id)(CONDITION), \
        (IR_block_id)(YES), (IR_block_id)(NO)))
#define IR_RETURN(VALUE, MEMORY) \
    IR_PROGRAM_STEP(IR_build_return( \
        &builder, (IR_value_id)(VALUE), (IR_value_id)(MEMORY)))
#define IR_RETURN_VOID(MEMORY) \
    IR_PROGRAM_STEP(IR_build_return( \
        &builder, IR_NO_VALUE, (IR_value_id)(MEMORY)))

#define PROGRAM_DECLARE_BLOCK IR_DECLARE_BLOCK
#define PROGRAM_BLOCK IR_BLOCK
#define PROGRAM_NEW_BLOCK IR_NEW_BLOCK
#define PROGRAM_ENTRY IR_ENTRY
#define PROGRAM_PARAMETER IR_PARAMETER
#define PROGRAM_POINTER_PARAMETER IR_POINTER_PARAMETER
#define PROGRAM_MEMORY_PARAMETER IR_MEMORY_PARAMETER
#define PROGRAM_CONSTANT IR_CONSTANT
#define PROGRAM_PHI2 IR_PHI2
#define PROGRAM_MEMORY_PHI2 IR_MEMORY_PHI2
#define PROGRAM_LOAD IR_LOAD
#define PROGRAM_STORE IR_STORE
#define PROGRAM_CALL0 IR_CALL0
#define PROGRAM_CALL1 IR_CALL1
#define PROGRAM_CALL2 IR_CALL2
#define PROGRAM_TARGET IR_TARGET
#define PROGRAM_JUMP IR_JUMP
#define PROGRAM_BRANCH IR_BRANCH
#define PROGRAM_RETURN IR_RETURN
#define PROGRAM_RETURN_VOID IR_RETURN_VOID

#define ARCHON_IR_DECLARE_BLOCK IR_DECLARE_BLOCK
#define ARCHON_IR_BLOCK IR_BLOCK
#define ARCHON_IR_NEW_BLOCK IR_NEW_BLOCK
#define ARCHON_IR_ENTRY IR_ENTRY
#define ARCHON_IR_PARAMETER IR_PARAMETER
#define ARCHON_IR_POINTER_PARAMETER IR_POINTER_PARAMETER
#define ARCHON_IR_MEMORY_PARAMETER IR_MEMORY_PARAMETER
#define ARCHON_IR_CONSTANT IR_CONSTANT
#define ARCHON_IR_PHI2 IR_PHI2
#define ARCHON_IR_MEMORY_PHI2 IR_MEMORY_PHI2
#define ARCHON_IR_LOAD IR_LOAD
#define ARCHON_IR_STORE IR_STORE
#define ARCHON_IR_CALL0 IR_CALL0
#define ARCHON_IR_CALL1 IR_CALL1
#define ARCHON_IR_CALL2 IR_CALL2
#define ARCHON_IR_TARGET IR_TARGET
#define ARCHON_IR_JUMP IR_JUMP
#define ARCHON_IR_BRANCH IR_BRANCH
#define ARCHON_IR_RETURN IR_RETURN
#define ARCHON_IR_RETURN_VOID IR_RETURN_VOID

#if defined(ARCHON_IR_PROGRAM_FILE)
#include ARCHON_IR_PROGRAM_FILE
#elif defined(ARCHON_IR_PROGRAM)
#include ARCHON_IR_PROGRAM
#endif

#undef ARCHON_IR_RETURN_VOID
#undef ARCHON_IR_RETURN
#undef ARCHON_IR_BRANCH
#undef ARCHON_IR_JUMP
#undef ARCHON_IR_TARGET
#undef ARCHON_IR_CALL2
#undef ARCHON_IR_CALL1
#undef ARCHON_IR_CALL0
#undef ARCHON_IR_STORE
#undef ARCHON_IR_LOAD
#undef ARCHON_IR_MEMORY_PHI2
#undef ARCHON_IR_PHI2
#undef ARCHON_IR_CONSTANT
#undef ARCHON_IR_MEMORY_PARAMETER
#undef ARCHON_IR_POINTER_PARAMETER
#undef ARCHON_IR_PARAMETER
#undef ARCHON_IR_ENTRY
#undef ARCHON_IR_NEW_BLOCK
#undef ARCHON_IR_BLOCK
#undef ARCHON_IR_DECLARE_BLOCK
#undef PROGRAM_RETURN_VOID
#undef PROGRAM_RETURN
#undef PROGRAM_BRANCH
#undef PROGRAM_JUMP
#undef PROGRAM_TARGET
#undef PROGRAM_CALL2
#undef PROGRAM_CALL1
#undef PROGRAM_CALL0
#undef PROGRAM_STORE
#undef PROGRAM_LOAD
#undef PROGRAM_MEMORY_PHI2
#undef PROGRAM_PHI2
#undef PROGRAM_CONSTANT
#undef PROGRAM_MEMORY_PARAMETER
#undef PROGRAM_POINTER_PARAMETER
#undef PROGRAM_PARAMETER
#undef PROGRAM_ENTRY
#undef PROGRAM_NEW_BLOCK
#undef PROGRAM_BLOCK
#undef PROGRAM_DECLARE_BLOCK
#undef IR_RETURN_VOID
#undef IR_RETURN
#undef IR_BRANCH
#undef IR_JUMP
#undef IR_TARGET
#undef IR_CALL2
#undef IR_CALL1
#undef IR_CALL0
#undef IR_STORE
#undef IR_LOAD
#undef IR_MEMORY_PHI2
#undef IR_PHI2
#undef IR_CONSTANT
#undef IR_MEMORY_PARAMETER
#undef IR_POINTER_PARAMETER
#undef IR_PARAMETER
#undef IR_ENTRY
#undef IR_NEW_BLOCK
#undef IR_BLOCK
#undef IR_DECLARE_BLOCK
#undef IR_PROGRAM_POINTER
#undef IR_PROGRAM_TYPE
#undef IR_PROGRAM_EXPECT
#undef IR_PROGRAM_STEP

    if (status != IR_OK) {
        IR_program_destroy(program);
        return IR_report(
            diagnostic, status, IR_NO_BLOCK, IR_NO_NODE,
            "Program.i builder failed");
    }
    status = IR_verify(program, diagnostic);
    if (status != IR_OK)
        IR_program_destroy(program);
    return status;
}

const char *IR_status_string(enum IR_status status) {
    switch (status) {
    case IR_OK: return "ok";
    case IR_INVALID_ARGUMENT: return "invalid argument";
    case IR_OUT_OF_MEMORY: return "out of memory";
    case IR_NOT_FOUND: return "not found";
    case IR_TYPE_MISMATCH: return "type mismatch";
    case IR_MALFORMED: return "malformed IR";
    case IR_BUFFER_TOO_SMALL: return "buffer too small";
    case IR_IO_ERROR: return "I/O error";
    case IR_UNSUPPORTED: return "unsupported IR form";
    default: return "unknown IR status";
    }
}

static enum IR_status IR_report(
    struct IR_diagnostic *diagnostic, enum IR_status status,
    IR_block_id block, IR_node_id node, const char *format, ...) {
    if (diagnostic != NULL) {
        va_list arguments;
        diagnostic->status = status;
        diagnostic->block = block;
        diagnostic->node = node;
        if (format == NULL) {
            diagnostic->message[0] = '\0';
        } else {
            va_start(arguments, format);
            (void)vsnprintf(
                diagnostic->message, sizeof(diagnostic->message),
                format, arguments);
            va_end(arguments);
        }
    }
    return status;
}

static int IR_is_terminator(enum IR_node_kind kind) {
    return kind == IR_NODE_JUMP || kind == IR_NODE_BRANCH ||
           kind == IR_NODE_RETURN;
}

static int IR_is_prefix_node(enum IR_node_kind kind) {
    return kind == IR_NODE_PARAMETER || kind == IR_NODE_MEMORY_PARAMETER ||
           kind == IR_NODE_PHI || kind == IR_NODE_MEMORY_PHI;
}

static int IR_cfg_has_edge(
    const struct IR_program *program, IR_block_id source, IR_block_id target) {
    size_t i;
    for (i = 0u; i < program->cfg.edge_count; ++i)
        if (program->cfg.edges[i].source == source &&
            program->cfg.edges[i].target == target)
            return 1;
    return 0;
}

static enum IR_status IR_verify_value_use(
    const struct IR_program *program, const size_t *positions,
    IR_value_id value, IR_block_id use_block, size_t use_position,
    struct IR_diagnostic *diagnostic, IR_node_id user) {
    const struct IR_value *row;
    const struct IR_node *producer;
    int dominates = 0;
    ArchonGraphStatus graph_status;
    if (!IR_value_valid(program, value))
        return IR_report(
            diagnostic, IR_NOT_FOUND, use_block, user,
            "value reference is outside the value table");
    row = &program->values[value];
    if ((size_t)row->producer >= program->node_count)
        return IR_report(
            diagnostic, IR_MALFORMED, use_block, user,
            "value has no valid producer");
    producer = &program->nodes[row->producer];
    if (producer->block == use_block) {
        if (positions[row->producer] >= use_position)
            return IR_report(
                diagnostic, IR_MALFORMED, use_block, user,
                "value definition does not precede its use");
        return IR_OK;
    }
    graph_status = archon_cfg_dominates(
        &program->cfg, producer->block, use_block, &dominates);
    if (graph_status != ARCHON_GRAPH_OK || !dominates)
        return IR_report(
            diagnostic, IR_MALFORMED, use_block, user,
            "value definition does not dominate its use");
    return IR_OK;
}

static enum IR_status IR_verify_phi(
    const struct IR_program *program, const struct IR_node *node,
    const size_t *positions, int memory,
    struct IR_diagnostic *diagnostic) {
    size_t predecessor_count =
        archon_graph_predecessor_count(&program->cfg, node->block);
    size_t i;
    if (node->incoming == NULL || node->incoming_count != predecessor_count ||
        node->incoming_count == 0u)
        return IR_report(
            diagnostic, IR_MALFORMED, node->block, node->id,
            "phi inputs must cover every CFG predecessor");
    for (i = 0u; i < node->incoming_count; ++i) {
        const struct IR_phi_input *input = &node->incoming[i];
        enum IR_status status;
        size_t j;
        if ((size_t)input->block >= program->block_count ||
            !IR_cfg_has_edge(program, input->block, node->block))
            return IR_report(
                diagnostic, IR_MALFORMED, node->block, node->id,
                "phi input is not associated with a predecessor");
        for (j = 0u; j < i; ++j)
            if (node->incoming[j].block == input->block)
                return IR_report(
                    diagnostic, IR_MALFORMED, node->block, node->id,
                    "phi has duplicate predecessor input");
        if (!IR_value_valid(program, input->value))
            return IR_report(
                diagnostic, IR_NOT_FOUND, node->block, node->id,
                "phi references an unknown value");
        if (memory) {
            if (!IR_memory_valid(program, input->value))
                return IR_report(
                    diagnostic, IR_TYPE_MISMATCH, node->block, node->id,
                    "memory-phi input is not a memory token");
        } else if (!IR_type_equal(
                       program->values[input->value].type, node->type)) {
            return IR_report(
                diagnostic, IR_TYPE_MISMATCH, node->block, node->id,
                "phi input type differs from result type");
        }
        status = IR_verify_value_use(
            program, positions, input->value, input->block, SIZE_MAX,
            diagnostic, node->id);
        if (status != IR_OK)
            return status;
    }
    return IR_OK;
}

static enum IR_status IR_verify_node_shape(
    const struct IR_program *program, const struct IR_node *node,
    struct IR_diagnostic *diagnostic) {
    int has_result = node->result != IR_NO_VALUE;
    int has_memory_result = node->memory_result != IR_NO_VALUE;
    int has_memory_input = node->memory_input != IR_NO_VALUE;
    const struct IR_operation_metadata *metadata;
    if (!IR_type_is_valid(node->type))
        return IR_report(
            diagnostic, IR_TYPE_MISMATCH, node->block, node->id,
            "node has an invalid type");
    if (has_result) {
        if (!IR_value_valid(program, node->result) ||
            program->values[node->result].producer != node->id ||
            !IR_type_equal(program->values[node->result].type, node->type) ||
            node->type.kind == IR_TYPE_MEMORY ||
            node->type.kind == IR_TYPE_VOID)
            return IR_report(
                diagnostic, IR_MALFORMED, node->block, node->id,
                "node result record is inconsistent");
    }
    if (has_memory_result) {
        if (!IR_memory_valid(program, node->memory_result) ||
            program->values[node->memory_result].producer != node->id)
            return IR_report(
                diagnostic, IR_MALFORMED, node->block, node->id,
                "node memory result record is inconsistent");
    }
    if (has_memory_input && !IR_memory_valid(program, node->memory_input))
        return IR_report(
            diagnostic, IR_TYPE_MISMATCH, node->block, node->id,
            "node memory input is not a memory token");
    if ((node->operand_count != 0u && node->operands == NULL) ||
        (node->incoming_count != 0u && node->incoming == NULL))
        return IR_report(
            diagnostic, IR_MALFORMED, node->block, node->id,
            "node has invalid owned arrays");
    switch (node->kind) {
    case IR_NODE_PARAMETER:
        if (!has_result || has_memory_result || has_memory_input ||
            node->operand_count != 0u || node->incoming_count != 0u)
            goto bad_shape;
        break;
    case IR_NODE_MEMORY_PARAMETER:
        if (has_result || !has_memory_result || has_memory_input ||
            node->operand_count != 0u || node->incoming_count != 0u ||
            node->type.kind != IR_TYPE_MEMORY)
            goto bad_shape;
        break;
    case IR_NODE_CONSTANT:
        if (!has_result || has_memory_result || has_memory_input ||
            node->operand_count != 0u || node->incoming_count != 0u ||
            node->type.width > 64u)
            goto bad_shape;
        break;
    case IR_NODE_PHI:
        if (!has_result || has_memory_result || has_memory_input ||
            node->operand_count != 0u || node->incoming_count == 0u)
            goto bad_shape;
        break;
    case IR_NODE_MEMORY_PHI:
        if (has_result || !has_memory_result || has_memory_input ||
            node->operand_count != 0u || node->incoming_count == 0u ||
            node->type.kind != IR_TYPE_MEMORY)
            goto bad_shape;
        break;
    case IR_NODE_LOAD:
        if (!has_result || has_memory_result || !has_memory_input ||
            node->operand_count != 1u || node->incoming_count != 0u ||
            node->effects != IR_EFFECT_READ_MEMORY ||
            !IR_alignment_valid(node->alignment))
            goto bad_shape;
        break;
    case IR_NODE_STORE:
        if (has_result || !has_memory_result || !has_memory_input ||
            node->operand_count != 2u || node->incoming_count != 0u ||
            node->effects != IR_EFFECT_WRITE_MEMORY ||
            !IR_alignment_valid(node->alignment))
            goto bad_shape;
        break;
    case IR_NODE_CALL:
        if ((node->name == NULL || node->name[0] == '\0') ||
            node->incoming_count != 0u || !IR_effects_valid(node->effects) ||
            has_memory_result != has_memory_input ||
            has_result != (node->type.kind != IR_TYPE_VOID))
            goto bad_shape;
        if (node->effects != IR_EFFECT_NONE && !has_memory_input)
            goto bad_shape;
        break;
    case IR_NODE_TARGET:
        metadata = IR_operation_metadata(node->operation);
        if (metadata == NULL || node->incoming_count != 0u ||
            metadata->result_count > 1u ||
            metadata->input_count != node->operand_count ||
            metadata->effects != node->effects ||
            has_result != (metadata->result_count == 1u) ||
            has_result != (node->type.kind != IR_TYPE_VOID) ||
            has_memory_result != (metadata->requires_sequence != 0u) ||
            has_memory_input != has_memory_result)
            goto bad_shape;
        if (has_result &&
            !IR_target_type_matches(
                node->type, IR_operation_result(node->operation, 0u)))
            return IR_report(
                diagnostic, IR_TYPE_MISMATCH, node->block, node->id,
                "target result type differs from its output operand");
        {
            size_t operand_index;
            for (operand_index = 0u;
                 operand_index < node->operand_count; ++operand_index) {
                if (!IR_value_valid(program, node->operands[operand_index]) ||
                    !IR_target_type_matches(
                        program->values[node->operands[operand_index]].type,
                        IR_operation_input(
                            node->operation, operand_index)))
                    return IR_report(
                        diagnostic, IR_TYPE_MISMATCH,
                        node->block, node->id,
                        "target input type differs from operand metadata");
            }
        }
        break;
    case IR_NODE_JUMP:
        if (has_result || has_memory_result || has_memory_input ||
            node->operand_count != 0u || node->incoming_count != 0u ||
            (size_t)node->target >= program->block_count ||
            node->alternative != IR_NO_BLOCK)
            goto bad_shape;
        break;
    case IR_NODE_BRANCH:
        if (has_result || has_memory_result || has_memory_input ||
            node->operand_count != 1u || node->incoming_count != 0u ||
            (size_t)node->target >= program->block_count ||
            (size_t)node->alternative >= program->block_count)
            goto bad_shape;
        break;
    case IR_NODE_RETURN:
        if (has_result || has_memory_result || node->operand_count > 1u ||
            node->incoming_count != 0u)
            goto bad_shape;
        break;
    default:
        goto bad_shape;
    }
    return IR_OK;

bad_shape:
    return IR_report(
        diagnostic, IR_MALFORMED, node->block, node->id,
        "node fields do not match its opcode");
}

enum IR_status IR_verify(
    const struct IR_program *program, struct IR_diagnostic *diagnostic) {
    unsigned char *seen_nodes = NULL;
    unsigned char *seen_memory_versions = NULL;
    size_t *positions = NULL;
    size_t memory_count = 0u;
    size_t i;
    enum IR_status status = IR_OK;
    char graph_message[128];
    if (program == NULL)
        return IR_report(
            diagnostic, IR_INVALID_ARGUMENT, IR_NO_BLOCK, IR_NO_NODE,
            "program is null");
    if (program->block_count > program->block_capacity ||
        program->node_count > program->node_capacity ||
        program->value_count > program->value_capacity ||
        (program->block_count != 0u && program->blocks == NULL) ||
        (program->node_count != 0u && program->nodes == NULL) ||
        (program->value_count != 0u && program->values == NULL))
        return IR_report(
            diagnostic, IR_MALFORMED, IR_NO_BLOCK, IR_NO_NODE,
            "program has invalid owned storage");
    if (archon_graph_validate(
            &program->cfg, graph_message, sizeof(graph_message)) !=
        ARCHON_GRAPH_OK)
        return IR_report(
            diagnostic, IR_MALFORMED, IR_NO_BLOCK, IR_NO_NODE,
            "invalid CFG: %s", graph_message);
    if (program->cfg.node_count != program->block_count)
        return IR_report(
            diagnostic, IR_MALFORMED, IR_NO_BLOCK, IR_NO_NODE,
            "CFG and block table have different sizes");
    if (program->block_count == 0u) {
        if (program->entry != IR_NO_BLOCK || program->node_count != 0u ||
            program->value_count != 0u)
            return IR_report(
                diagnostic, IR_MALFORMED, IR_NO_BLOCK, IR_NO_NODE,
                "empty program has residual IR");
        return IR_report(
            diagnostic, IR_OK, IR_NO_BLOCK, IR_NO_NODE, "ok");
    }
    if ((size_t)program->entry >= program->block_count ||
        program->cfg.entry != program->entry)
        return IR_report(
            diagnostic, IR_MALFORMED, IR_NO_BLOCK, IR_NO_NODE,
            "program has no valid entry block");
    seen_nodes = (unsigned char *)calloc(program->node_count, 1u);
    positions = (size_t *)malloc(program->node_count * sizeof(*positions));
    seen_memory_versions =
        (unsigned char *)calloc(program->next_memory_version, 1u);
    if ((seen_nodes == NULL || positions == NULL) &&
        program->node_count != 0u) {
        status = IR_report(
            diagnostic, IR_OUT_OF_MEMORY, IR_NO_BLOCK, IR_NO_NODE,
            "cannot allocate verifier state");
        goto done;
    }
    if (seen_memory_versions == NULL && program->next_memory_version != 0u) {
        status = IR_report(
            diagnostic, IR_OUT_OF_MEMORY, IR_NO_BLOCK, IR_NO_NODE,
            "cannot allocate memory-version verifier state");
        goto done;
    }
    for (i = 0u; i < program->value_count; ++i) {
        const struct IR_value *value = &program->values[i];
        if (value->id != (IR_value_id)i ||
            !IR_type_is_valid(value->type) ||
            (size_t)value->producer >= program->node_count ||
            value->name == NULL) {
            status = IR_report(
                diagnostic, IR_MALFORMED, IR_NO_BLOCK, value->producer,
                "invalid value table entry");
            goto done;
        }
        if (program->nodes[value->producer].result != value->id &&
            program->nodes[value->producer].memory_result != value->id) {
            status = IR_report(
                diagnostic, IR_MALFORMED, IR_NO_BLOCK, value->producer,
                "value is not an output of its producer");
            goto done;
        }
        if (value->type.kind == IR_TYPE_MEMORY) {
            if (value->memory_version >= program->next_memory_version ||
                seen_memory_versions[value->memory_version] != 0u) {
                status = IR_report(
                    diagnostic, IR_MALFORMED, IR_NO_BLOCK, value->producer,
                    "invalid or duplicate memory version");
                goto done;
            }
            seen_memory_versions[value->memory_version] = 1u;
            ++memory_count;
        } else if (value->memory_version != UINT32_MAX) {
            status = IR_report(
                diagnostic, IR_MALFORMED, IR_NO_BLOCK, value->producer,
                "ordinary value carries a memory version");
            goto done;
        }
    }
    if (memory_count != (size_t)program->next_memory_version) {
        status = IR_report(
            diagnostic, IR_MALFORMED, IR_NO_BLOCK, IR_NO_NODE,
            "memory versions are not dense");
        goto done;
    }
    for (i = 0u; i < program->block_count; ++i) {
        const struct IR_block *block = &program->blocks[i];
        size_t j;
        int ordinary_seen = 0;
        if (block->id != (IR_block_id)i || block->name == NULL ||
            block->node_count > block->node_capacity ||
            (block->node_count != 0u && block->nodes == NULL)) {
            status = IR_report(
                diagnostic, IR_MALFORMED, (IR_block_id)i, IR_NO_NODE,
                "invalid block table entry");
            goto done;
        }
        if (block->node_count == 0u) {
            status = IR_report(
                diagnostic, IR_MALFORMED, block->id, IR_NO_NODE,
                "nonempty CFG block has no terminator");
            goto done;
        }
        for (j = 0u; j < block->node_count; ++j) {
            IR_node_id id = block->nodes[j];
            const struct IR_node *node;
            if ((size_t)id >= program->node_count || seen_nodes[id] != 0u) {
                status = IR_report(
                    diagnostic, IR_MALFORMED, block->id, id,
                    "block contains an invalid or duplicate node");
                goto done;
            }
            node = &program->nodes[id];
            if (node->id != id || node->block != block->id) {
                status = IR_report(
                    diagnostic, IR_MALFORMED, block->id, id,
                    "node ownership is inconsistent");
                goto done;
            }
            seen_nodes[id] = 1u;
            positions[id] = j;
            if (!IR_is_prefix_node(node->kind))
                ordinary_seen = 1;
            else if (ordinary_seen) {
                status = IR_report(
                    diagnostic, IR_MALFORMED, block->id, id,
                    "parameter or phi follows an ordinary node");
                goto done;
            }
            if (IR_is_terminator(node->kind) !=
                (j + 1u == block->node_count)) {
                status = IR_report(
                    diagnostic, IR_MALFORMED, block->id, id,
                    "terminator must be the final node in its block");
                goto done;
            }
        }
    }
    for (i = 0u; i < program->node_count; ++i)
        if (seen_nodes[i] == 0u) {
            status = IR_report(
                diagnostic, IR_MALFORMED, IR_NO_BLOCK, (IR_node_id)i,
                "node is not owned by a block");
            goto done;
        }
    for (i = 0u; i < program->node_count; ++i) {
        const struct IR_node *node = &program->nodes[i];
        size_t j;
        size_t position = positions[i];
        status = IR_verify_node_shape(program, node, diagnostic);
        if (status != IR_OK)
            goto done;
        for (j = 0u; j < node->operand_count; ++j) {
            status = IR_verify_value_use(
                program, positions, node->operands[j], node->block,
                position, diagnostic, node->id);
            if (status != IR_OK)
                goto done;
        }
        if (node->memory_input != IR_NO_VALUE) {
            status = IR_verify_value_use(
                program, positions, node->memory_input, node->block,
                position, diagnostic, node->id);
            if (status != IR_OK)
                goto done;
        }
        if (node->kind == IR_NODE_PHI ||
            node->kind == IR_NODE_MEMORY_PHI) {
            status = IR_verify_phi(
                program, node, positions,
                node->kind == IR_NODE_MEMORY_PHI, diagnostic);
            if (status != IR_OK)
                goto done;
        }
        if (node->kind == IR_NODE_LOAD || node->kind == IR_NODE_STORE) {
            if (!IR_address_type(
                    program->values[node->operands[0]].type)) {
                status = IR_report(
                    diagnostic, IR_TYPE_MISMATCH, node->block, node->id,
                    "memory address has an invalid type");
                goto done;
            }
        }
        if (node->kind == IR_NODE_BRANCH) {
            struct IR_type condition =
                program->values[node->operands[0]].type;
            if (condition.width != 1u ||
                (condition.kind != IR_TYPE_BITS &&
                 condition.kind != IR_TYPE_UINT)) {
                status = IR_report(
                    diagnostic, IR_TYPE_MISMATCH, node->block, node->id,
                    "branch condition is not a one-bit value");
                goto done;
            }
        }
        if (node->kind == IR_NODE_JUMP) {
            if (archon_graph_successor_count(
                    &program->cfg, node->block) != 1u ||
                !IR_cfg_has_edge(program, node->block, node->target)) {
                status = IR_report(
                    diagnostic, IR_MALFORMED, node->block, node->id,
                    "jump and CFG successors disagree");
                goto done;
            }
        } else if (node->kind == IR_NODE_BRANCH) {
            if (archon_graph_successor_count(
                    &program->cfg, node->block) != 2u ||
                !IR_cfg_has_edge(program, node->block, node->target) ||
                !IR_cfg_has_edge(program, node->block, node->alternative)) {
                status = IR_report(
                    diagnostic, IR_MALFORMED, node->block, node->id,
                    "branch and CFG successors disagree");
                goto done;
            }
        } else if (node->kind == IR_NODE_RETURN &&
                   archon_graph_successor_count(
                       &program->cfg, node->block) != 0u) {
            status = IR_report(
                diagnostic, IR_MALFORMED, node->block, node->id,
                "return block has CFG successors");
            goto done;
        }
    }
    status = IR_report(
        diagnostic, IR_OK, IR_NO_BLOCK, IR_NO_NODE, "ok");

done:
    free(seen_nodes);
    free(seen_memory_versions);
    free(positions);
    return status;
}

static int IR_address_type(struct IR_type type) {
    return type.kind == IR_TYPE_POINTER || type.kind == IR_TYPE_BITS ||
           type.kind == IR_TYPE_UINT;
}

static int IR_alignment_valid(uint32_t alignment) {
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

enum IR_status IR_build_load(
    struct IR_builder *builder, struct IR_type type, IR_value_id address,
    IR_value_id memory, uint32_t alignment, const char *name,
    IR_value_id *result) {
    struct IR_node node;
    enum IR_status status;
    IR_value_id operands[1];
    if (builder == NULL || builder->program == NULL ||
        !IR_type_is_valid(type) || type.kind == IR_TYPE_VOID ||
        type.kind == IR_TYPE_MEMORY || result == NULL ||
        !IR_value_valid(builder->program, address) ||
        !IR_address_type(builder->program->values[address].type) ||
        !IR_memory_valid(builder->program, memory) ||
        !IR_alignment_valid(alignment))
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_LOAD);
    operands[0] = address;
    status = IR_copy_values(
        builder->program, operands, 1u, &node.operands);
    if (status != IR_OK)
        return status;
    node.operand_count = 1u;
    node.memory_input = memory;
    node.type = type;
    node.alignment = alignment;
    node.effects = IR_EFFECT_READ_MEMORY;
    node.name = IR_copy_string(name);
    if (node.name == NULL) {
        free(node.operands);
        return IR_OUT_OF_MEMORY;
    }
    status = IR_append_node(
        builder, &node, 1, type, name, 0, result, NULL);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

enum IR_status IR_build_store(
    struct IR_builder *builder, IR_value_id address, IR_value_id value,
    IR_value_id memory, uint32_t alignment, IR_value_id *memory_result) {
    struct IR_node node;
    enum IR_status status;
    IR_value_id operands[2];
    if (builder == NULL || builder->program == NULL ||
        !IR_value_valid(builder->program, address) ||
        !IR_address_type(builder->program->values[address].type) ||
        !IR_value_valid(builder->program, value) ||
        builder->program->values[value].type.kind == IR_TYPE_MEMORY ||
        !IR_memory_valid(builder->program, memory) ||
        !IR_alignment_valid(alignment) || memory_result == NULL)
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_STORE);
    operands[0] = address;
    operands[1] = value;
    status = IR_copy_values(
        builder->program, operands, 2u, &node.operands);
    if (status != IR_OK)
        return status;
    node.operand_count = 2u;
    node.memory_input = memory;
    node.type = IR_memory_type();
    node.alignment = alignment;
    node.effects = IR_EFFECT_WRITE_MEMORY;
    status = IR_append_node(
        builder, &node, 0, node.type, NULL, 1, NULL, memory_result);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

static int IR_effects_valid(unsigned effects) {
    const unsigned all = IR_EFFECT_READ_MEMORY | IR_EFFECT_WRITE_MEMORY |
        IR_EFFECT_SIDE_EFFECT | IR_EFFECT_MAY_TRAP |
        IR_EFFECT_READ_SCALAR | IR_EFFECT_WRITE_SCALAR |
        IR_EFFECT_READ_STACK | IR_EFFECT_WRITE_STACK | IR_EFFECT_OPAQUE;
    return (effects & ~all) == 0u;
}

enum IR_status IR_build_call(
    struct IR_builder *builder, const char *callee,
    const IR_value_id *arguments, size_t argument_count,
    struct IR_type result_type, IR_value_id memory, unsigned effects,
    IR_value_id *result, IR_value_id *memory_result) {
    struct IR_node node;
    enum IR_status status;
    int has_result;
    int has_memory;
    if (builder == NULL || builder->program == NULL || callee == NULL ||
        *callee == '\0' || !IR_type_is_valid(result_type) ||
        result_type.kind == IR_TYPE_MEMORY || !IR_effects_valid(effects))
        return IR_INVALID_ARGUMENT;
    has_result = result_type.kind != IR_TYPE_VOID;
    has_memory = memory != IR_NO_VALUE;
    if ((has_result && result == NULL) ||
        (has_memory && (!IR_memory_valid(builder->program, memory) ||
                        memory_result == NULL)) ||
        (!has_memory && effects != IR_EFFECT_NONE))
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_CALL);
    status = IR_copy_values(
        builder->program, arguments, argument_count, &node.operands);
    if (status != IR_OK)
        return status;
    node.operand_count = argument_count;
    node.memory_input = memory;
    node.type = result_type;
    node.effects = effects;
    node.name = IR_copy_string(callee);
    if (node.name == NULL) {
        free(node.operands);
        return IR_OUT_OF_MEMORY;
    }
    status = IR_append_node(
        builder, &node, has_result, result_type, callee, has_memory,
        result, memory_result);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

static int IR_target_type_matches(
    struct IR_type type, const struct IR_operand_metadata *operand) {
    if (operand == NULL || type.width != operand->value_width ||
        type.address_space != 0u)
        return 0;
    if (operand->kind == IR_OPERAND_REGISTER)
        return type.kind == IR_TYPE_BITS || type.kind == IR_TYPE_UINT ||
               type.kind == IR_TYPE_SINT;
    if (operand->is_signed)
        return type.kind == IR_TYPE_BITS || type.kind == IR_TYPE_SINT;
    return type.kind == IR_TYPE_BITS || type.kind == IR_TYPE_UINT;
}

enum IR_status IR_build_target(
    struct IR_builder *builder, enum IR_operation operation,
    const IR_value_id *operands, size_t operand_count,
    struct IR_type result_type, IR_value_id memory,
    IR_value_id *result, IR_value_id *memory_result) {
    const struct IR_operation_metadata *metadata =
        IR_operation_metadata(operation);
    struct IR_node node;
    enum IR_status status;
    int has_result;
    int has_memory;
    size_t index;
    if (builder == NULL || builder->program == NULL || metadata == NULL ||
        !IR_type_is_valid(result_type) ||
        result_type.kind == IR_TYPE_MEMORY)
        return IR_INVALID_ARGUMENT;
    if (metadata->result_count > 1u)
        return IR_UNSUPPORTED;
    if (metadata->input_count != operand_count ||
        (operand_count != 0u && operands == NULL))
        return IR_INVALID_ARGUMENT;
    has_result = metadata->result_count == 1u;
    if (has_result != (result_type.kind != IR_TYPE_VOID))
        return IR_TYPE_MISMATCH;
    if (has_result &&
        !IR_target_type_matches(
            result_type, IR_operation_result(operation, 0u)))
        return IR_TYPE_MISMATCH;
    for (index = 0u; index < operand_count; ++index) {
        if (!IR_value_valid(builder->program, operands[index]))
            return IR_NOT_FOUND;
        if (!IR_target_type_matches(
                builder->program->values[operands[index]].type,
                IR_operation_input(operation, index)))
            return IR_TYPE_MISMATCH;
    }
    has_memory = metadata->requires_sequence != 0u;
    if ((has_result && result == NULL) ||
        (has_memory &&
         (!IR_memory_valid(builder->program, memory) ||
          memory_result == NULL)) ||
        (!has_memory && memory != IR_NO_VALUE))
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_TARGET);
    status = IR_copy_values(
        builder->program, operands, operand_count, &node.operands);
    if (status != IR_OK)
        return status;
    node.operand_count = operand_count;
    node.memory_input = memory;
    node.type = result_type;
    node.effects = metadata->effects;
    node.operation = operation;
    node.name = IR_copy_string(metadata->name);
    if (node.name == NULL) {
        free(node.operands);
        return IR_OUT_OF_MEMORY;
    }
    status = IR_append_node(
        builder, &node, has_result, result_type, metadata->name,
        has_memory, result, memory_result);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

static enum IR_status IR_graph_status(ArchonGraphStatus status) {
    if (status == ARCHON_GRAPH_OK)
        return IR_OK;
    if (status == ARCHON_GRAPH_OUT_OF_MEMORY)
        return IR_OUT_OF_MEMORY;
    return IR_MALFORMED;
}

static void IR_rollback_cfg_edges(
    struct IR_program *program, size_t edge_count) {
    while (program->cfg.edge_count > edge_count) {
        size_t index = --program->cfg.edge_count;
        free(program->cfg.edges[index].label);
        memset(&program->cfg.edges[index], 0, sizeof(program->cfg.edges[index]));
    }
}

enum IR_status IR_build_jump(
    struct IR_builder *builder, IR_block_id target) {
    struct IR_node node;
    enum IR_status status;
    ArchonGraphStatus graph_status;
    ArchonGraphEdgeId edge;
    size_t old_edge_count;
    if (builder == NULL || builder->program == NULL ||
        (size_t)target >= builder->program->block_count)
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_JUMP);
    node.target = target;
    old_edge_count = builder->program->cfg.edge_count;
    graph_status = archon_cfg_add_edge(
        &builder->program->cfg, builder->block, target, "jump", &edge);
    if (graph_status != ARCHON_GRAPH_OK) {
        IR_rollback_cfg_edges(builder->program, old_edge_count);
        return IR_graph_status(graph_status);
    }
    status = IR_append_node(
        builder, &node, 0, node.type, NULL, 0, NULL, NULL);
    if (status != IR_OK) {
        IR_rollback_cfg_edges(builder->program, old_edge_count);
        return status;
    }
    return IR_OK;
}

enum IR_status IR_build_branch(
    struct IR_builder *builder, IR_value_id condition,
    IR_block_id if_true, IR_block_id if_false) {
    struct IR_node node;
    enum IR_status status;
    ArchonGraphStatus graph_status;
    ArchonGraphEdgeId edge;
    IR_value_id operands[1];
    struct IR_type condition_type;
    size_t old_edge_count;
    if (builder == NULL || builder->program == NULL ||
        !IR_value_valid(builder->program, condition) ||
        (size_t)if_true >= builder->program->block_count ||
        (size_t)if_false >= builder->program->block_count)
        return IR_INVALID_ARGUMENT;
    condition_type = builder->program->values[condition].type;
    if (condition_type.width != 1u ||
        (condition_type.kind != IR_TYPE_BITS &&
         condition_type.kind != IR_TYPE_UINT))
        return IR_TYPE_MISMATCH;
    IR_node_defaults(&node, IR_NODE_BRANCH);
    operands[0] = condition;
    status = IR_copy_values(
        builder->program, operands, 1u, &node.operands);
    if (status != IR_OK)
        return status;
    node.operand_count = 1u;
    node.target = if_true;
    node.alternative = if_false;
    old_edge_count = builder->program->cfg.edge_count;
    graph_status = archon_cfg_add_edge(
        &builder->program->cfg, builder->block, if_true, "true", &edge);
    if (graph_status == ARCHON_GRAPH_OK)
        graph_status = archon_cfg_add_edge(
            &builder->program->cfg, builder->block, if_false, "false", &edge);
    if (graph_status != ARCHON_GRAPH_OK) {
        IR_rollback_cfg_edges(builder->program, old_edge_count);
        IR_clear_node(&node);
        return IR_graph_status(graph_status);
    }
    status = IR_append_node(
        builder, &node, 0, node.type, NULL, 0, NULL, NULL);
    if (status != IR_OK) {
        IR_rollback_cfg_edges(builder->program, old_edge_count);
        IR_clear_node(&node);
        return status;
    }
    return IR_OK;
}

enum IR_status IR_build_return(
    struct IR_builder *builder, IR_value_id value, IR_value_id memory) {
    struct IR_node node;
    enum IR_status status;
    if (builder == NULL || builder->program == NULL ||
        (value != IR_NO_VALUE &&
         (!IR_value_valid(builder->program, value) ||
          builder->program->values[value].type.kind == IR_TYPE_MEMORY)) ||
        (memory != IR_NO_VALUE && !IR_memory_valid(builder->program, memory)))
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_RETURN);
    if (value != IR_NO_VALUE) {
        status = IR_copy_values(
            builder->program, &value, 1u, &node.operands);
        if (status != IR_OK)
            return status;
        node.operand_count = 1u;
    }
    node.memory_input = memory;
    status = IR_append_node(
        builder, &node, 0, node.type, NULL, 0, NULL, NULL);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

enum IR_status IR_build_memory_parameter(
    struct IR_builder *builder, const char *name, IR_value_id *result) {
    struct IR_node node;
    enum IR_status status;
    if (result == NULL)
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_MEMORY_PARAMETER);
    node.type = IR_memory_type();
    node.name = IR_copy_string(name);
    if (node.name == NULL)
        return IR_OUT_OF_MEMORY;
    status = IR_append_node(
        builder, &node, 0, node.type, name, 1, NULL, result);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

enum IR_status IR_build_constant(
    struct IR_builder *builder, struct IR_type type, uint64_t value,
    const char *name, IR_value_id *result) {
    struct IR_node node;
    enum IR_status status;
    if (!IR_type_is_valid(type) || type.kind == IR_TYPE_VOID ||
        type.kind == IR_TYPE_MEMORY || type.kind == IR_TYPE_CONTROL ||
        type.width > 64u || result == NULL)
        return IR_INVALID_ARGUMENT;
    if (type.width < 64u && type.kind != IR_TYPE_SINT &&
        value > (UINT64_C(1) << type.width) - UINT64_C(1))
        return IR_TYPE_MISMATCH;
    IR_node_defaults(&node, IR_NODE_CONSTANT);
    node.type = type;
    node.immediate = value;
    node.name = IR_copy_string(name);
    if (node.name == NULL)
        return IR_OUT_OF_MEMORY;
    status = IR_append_node(
        builder, &node, 1, type, name, 0, result, NULL);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

enum IR_status IR_build_phi(
    struct IR_builder *builder, struct IR_type type,
    const struct IR_phi_input *incoming, size_t incoming_count,
    const char *name, IR_value_id *result) {
    struct IR_node node;
    enum IR_status status;
    size_t i;
    if (builder == NULL || builder->program == NULL ||
        !IR_type_is_valid(type) || type.kind == IR_TYPE_VOID ||
        type.kind == IR_TYPE_MEMORY || result == NULL)
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_PHI);
    status = IR_copy_incoming(
        builder->program, incoming, incoming_count, &node.incoming, 0);
    if (status != IR_OK)
        return status;
    for (i = 0u; i < incoming_count; ++i) {
        if (!IR_type_equal(
                builder->program->values[incoming[i].value].type, type)) {
            free(node.incoming);
            return IR_TYPE_MISMATCH;
        }
    }
    node.incoming_count = incoming_count;
    node.type = type;
    node.name = IR_copy_string(name);
    if (node.name == NULL) {
        free(node.incoming);
        return IR_OUT_OF_MEMORY;
    }
    status = IR_append_node(
        builder, &node, 1, type, name, 0, result, NULL);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

enum IR_status IR_build_memory_phi(
    struct IR_builder *builder, const struct IR_phi_input *incoming,
    size_t incoming_count, const char *name, IR_value_id *result) {
    struct IR_node node;
    enum IR_status status;
    if (builder == NULL || builder->program == NULL || result == NULL)
        return IR_INVALID_ARGUMENT;
    IR_node_defaults(&node, IR_NODE_MEMORY_PHI);
    status = IR_copy_incoming(
        builder->program, incoming, incoming_count, &node.incoming, 1);
    if (status != IR_OK)
        return status;
    node.incoming_count = incoming_count;
    node.type = IR_memory_type();
    node.name = IR_copy_string(name);
    if (node.name == NULL) {
        free(node.incoming);
        return IR_OUT_OF_MEMORY;
    }
    status = IR_append_node(
        builder, &node, 0, node.type, name, 1, NULL, result);
    if (status != IR_OK)
        IR_clear_node(&node);
    return status;
}

"""
