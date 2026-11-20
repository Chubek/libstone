"""Generate the final assembly emitter and pipeline driver.

The emitter is data driven: instruction spellings, operand order, register
names, and immediate constraints are embedded from the selected ArchDB.  It
does not fall back to printing an opcode when a dialect has no validated
template; an unsupported dialect is a diagnostic unless an explicit dialect
hook handles the instruction.
"""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
import re

from ..diagnostics import ArchonError
from .macros import c_identifier


NAME = "emitter"
ALIASES = ("emit", "assembly", "asm")

_RISCV = re.compile(r"(?:RV|RISCV)[_-]?(?:32|64)?", re.IGNORECASE)
_AARCH64 = re.compile(r"(?:AARCH64|ARM64)", re.IGNORECASE)
_MIPS = re.compile(r"MIPS(?:32)?", re.IGNORECASE)


def _oracle_enabled(context: object) -> bool:
    if not isinstance(context, Mapping):
        return True
    options = context.get("options", {})
    if options is None:
        options = {}
    if not isinstance(options, Mapping):
        raise ArchonError("emitter options must be an object")
    enabled = options.get("oracle", True)
    if type(enabled) is not bool:
        raise ArchonError("emitter option 'oracle' must be boolean")
    return enabled


def _get(context, *names):
    if isinstance(context, Mapping):
        for name in names:
            if name in context:
                return context[name]
    for name in names:
        value = getattr(context, name, None)
        if value is not None:
            return value
    return None


def _database(context) -> dict:
    value = context if isinstance(context, (str, Path)) else _get(
        context, "database", "db", "archdb", "target"
    )
    if value is None:
        values = _get(
            context,
            "architectures",
            "databases",
            "archdbs",
            "selected_databases",
            "selected_archdbs",
            "targets",
        )
        if isinstance(values, (list, tuple)):
            if len(values) != 1:
                raise ArchonError(
                    "the emitter generator requires exactly one compiled ArchDB"
                )
            value = values[0]
    if value is None and isinstance(context, Mapping) and "instructions" in context:
        value = context
    if isinstance(value, (str, Path)):
        from ..database import load

        value = load(value)
    if not isinstance(value, dict):
        raise ArchonError("the emitter generator requires a compiled ArchDB")
    # Validation is intentionally delegated to the public Target constructor;
    # it catches both schema errors and encoding relationships.
    from ..runtime import Target

    Target(value)
    return value


def _instruction_primitive(instruction: Mapping) -> str | None:
    """Return only an exact source operation established by ArchSem.

    The selector's semantic-pattern recognizer checks the complete single-result
    expression, operand order and kinds, declared reads/writes, and conversion
    shape.  Sharing that recognizer keeps final emission from reintroducing the
    mnemonic inference deliberately excluded at the ArchDB boundary.
    """

    from .isel import _semantic_pattern

    pattern = _semantic_pattern(instruction)
    if pattern is None or pattern.get("explicit"):
        return None
    source = pattern.get("source")
    return source if isinstance(source, str) else None


def _c_string(value: str | None) -> str:
    if value is None:
        return "NULL"
    data = value.encode("utf-8")
    pieces = ['"']
    for byte in data:
        if byte == 0x22:
            pieces.append(r"\"")
        elif byte == 0x5C:
            pieces.append(r"\\")
        elif 0x20 <= byte <= 0x7E:
            pieces.append(chr(byte))
        else:
            pieces.append(f"\\{byte:03o}")
    pieces.append('"')
    return "".join(pieces)


def _dialect_table(db: dict, name: str):
    tables = db.get("assembly_dialects", db.get("dialects", {}))
    if isinstance(tables, Mapping):
        value = tables.get(name)
        if isinstance(value, Mapping):
            return value
    return {}


def _explicit_template(db: dict, instruction: dict, dialect: str):
    for container_name in ("assembly_dialects", "dialects"):
        container = instruction.get(container_name)
        if isinstance(container, Mapping):
            value = container.get(dialect)
            if isinstance(value, str):
                return value
            if isinstance(value, Mapping) and isinstance(value.get("template"), str):
                return value["template"]
    table = _dialect_table(db, dialect)
    templates = table.get("instructions")
    if isinstance(templates, Mapping):
        value = templates.get(instruction["name"])
        if isinstance(value, str):
            return value
    return None


def _validated_builtin(db: dict, dialect: str) -> bool:
    table = _dialect_table(db, dialect)
    if table.get("supported") is True:
        return True
    architecture = db.get("architecture", "")
    # These profiles' canonical Archon syntax is intentionally the overlap
    # supported by GAS and Sandstone's checked-in architecture profiles.
    # x86 is excluded: Intel and AT&T spellings cannot be inferred safely.
    patterns = (_RISCV, _AARCH64, _MIPS)
    return any(pattern.fullmatch(architecture) for pattern in patterns)


def _dialect_supported(db: dict, dialect: str, instructions: list[dict]) -> bool:
    if _validated_builtin(db, dialect):
        return True
    return bool(instructions) and all(
        isinstance(instruction, Mapping)
        and _explicit_template(db, instruction, dialect) is not None
        for instruction in instructions
    )


def _template(db: dict, instruction: dict, dialect: str):
    explicit = _explicit_template(db, instruction, dialect)
    if explicit is not None:
        return explicit
    if not _validated_builtin(db, dialect):
        return None
    assembly = instruction.get("assembly")
    return assembly if isinstance(assembly, str) else None


def _metadata(db: dict) -> dict[str, str]:
    from .isel import _legacy_registers_supported

    # Also reject these forms in directly supplied Regallocated.i artifacts,
    # which need not have passed through the selector.
    instructions = sorted(
        (row for row in db["instructions"] if _legacy_registers_supported(row)),
        key=lambda row: row["name"],
    )
    operands = []
    rows = []
    for instruction in instructions:
        first = len(operands)
        for operand in instruction["operands"]:
            operands.append(
                "    {%s, %s, %s, %uu, %uu},"
                % (
                    _c_string(operand["name"]),
                    "EMITTER_META_REGISTER"
                    if operand["kind"] == "reg"
                    else "EMITTER_META_IMMEDIATE",
                    {
                        "in": "EMITTER_ROLE_INPUT",
                        "out": "EMITTER_ROLE_OUTPUT",
                        "inout": "EMITTER_ROLE_INOUT",
                    }.get(operand.get("direction"), "EMITTER_ROLE_INPUT"),
                    operand["width"],
                    int(operand["signed"]),
                )
            )
        rows.append(
            "    {%s, %s, %s, %s, %s, %uu, %uu, %uu},"
            % (
                _c_string(instruction["name"]),
                _c_string(c_identifier(instruction["name"])),
                _c_string(_instruction_primitive(instruction)),
                _c_string(_template(db, instruction, "gas")),
                _c_string(_template(db, instruction, "sandstone")),
                first,
                len(instruction["operands"]),
                max(1, (int(instruction.get("width", 8)) + 7) // 8),
            )
        )
    if not operands:
        operands.append(
            "    {NULL, EMITTER_META_IMMEDIATE, EMITTER_ROLE_INPUT, 0u, 0u},"
        )
    if not rows:
        rows.append("    {NULL, NULL, NULL, NULL, NULL, 0u, 0u, 0u},")

    registers = db.get("registers")
    prefix = registers["prefix"] if registers is not None else ""
    count = registers["count"] if registers is not None else 0
    aliases = []
    if registers is not None:
        # Prefer a stable numbered spelling.  Aliases remain metadata for
        # diagnostics; allocator numbers never become unchecked source text.
        for name, index in sorted(registers.get("aliases", {}).items()):
            aliases.append("    {%s, %uu}," % (_c_string(name), index))
    if not aliases:
        aliases.append("    {NULL, 0u},")

    architecture = str(db.get("architecture", "unknown"))
    target_names = [architecture, architecture.lower()]
    if _RISCV.fullmatch(architecture):
        target_names.append("riscv64" if "64" in architecture else "riscv")
    target_names = list(dict.fromkeys(target_names))
    lua_names = ", ".join(_c_string(name) for name in target_names)
    return {
        "@TARGET@": _c_string(architecture),
        "@TARGET_LUA_NAMES@": lua_names,
        "@REGISTER_PREFIX@": _c_string(prefix),
        "@REGISTER_COUNT@": str(count),
        "@OPERANDS@": "\n".join(operands),
        "@INSTRUCTIONS@": "\n".join(rows),
        "@INSTRUCTION_COUNT@": str(len(instructions)),
        "@ALIASES@": "\n".join(aliases),
        "@ALIAS_COUNT@": str(0 if registers is None else len(registers.get("aliases", {}))),
        "@GAS_SUPPORTED@": str(int(_dialect_supported(db, "gas", instructions))),
        "@SANDSTONE_SUPPORTED@": str(int(_dialect_supported(db, "sandstone", instructions))),
    }


def _render(template: str, replacements: Mapping[str, str]) -> str:
    for marker, value in replacements.items():
        template = template.replace(marker, value)
    leftovers = sorted(set(re.findall(r"@[A-Z_]+@", template)))
    if leftovers:
        raise AssertionError(f"unexpanded emitter markers: {', '.join(leftovers)}")
    return template


def emit(context) -> dict[str, str]:
    """Generate ``Emitter.h``, ``Emitter.c``, and ``Driver.c``."""

    db = _database(context)
    replacements = _metadata(db)
    oracle_enabled = _oracle_enabled(context)
    oracle_include = '#include "Oracle.h"\n' if oracle_enabled else ""
    replacements["@ORACLE_INCLUDE@"] = oracle_include
    replacements["@ORACLE_OPTION_FIELD@"] = (
        "    const ArchonOracleOptions *oracle;\n"
        if oracle_enabled
        else ""
    )
    replacements["@ORACLE_SUPPORT@"] = _ORACLE_SUPPORT if oracle_enabled else ""
    replacements["@ORACLE_PREPARE@"] = (
        "    status = Emitter_apply_oracle_annotations(\n"
        "        &context, input_path);\n"
        "    if (status != EMITTER_STATUS_OK)\n"
        "        goto done;\n"
        if oracle_enabled
        else ""
    )
    replacements["@ORACLE_COMMENT@"] = (
        "    if (context->oracle_comment[0] != '\\0') {\n"
        "        const char *prefix = context->dialect == EMITTER_DIALECT_GAS\n"
        '            ? "# Oracle: " : "; Oracle: ";\n'
        "        if (fprintf(context->output, \"%s%s\\n\", prefix,\n"
        "                context->oracle_comment) < 0)\n"
        "            return Emitter_fail(\n"
        "                context, EMITTER_STATUS_IO_ERROR, 0u,\n"
        '                "cannot write Oracle comment");\n'
        "    }\n"
        if oracle_enabled
        else ""
    )
    ir = _get(context, "ir")
    driver = (
        _UNIVERSAL_DRIVER_C
        if ir is not None
        else _EMITTER_ONLY_DRIVER_C
    )
    driver = driver.replace("@ORACLE_INCLUDE@", oracle_include)
    driver = driver.replace(
        "@ORACLE_DRIVER_LOCAL@",
        "    ArchonOracleOptions oracle;\n"
        if oracle_enabled and ir is not None
        else "",
    )
    driver = driver.replace(
        "@ORACLE_DRIVER_INIT@",
        (
            "    archon_oracle_options_init(&oracle);\n"
            '    oracle.socket_path = getenv("ARCHON_ORACLE_SOCKET");\n'
            "    pipeline.oracle = &oracle;\n"
            "    emitter.oracle = &oracle;\n"
        )
        if oracle_enabled and ir is not None
        else "",
    )
    driver = driver.replace(
        "@DRIVER_ARTIFACTS@",
        (
            _ORACLE_DRIVER_ARTIFACTS
            if oracle_enabled
            else _DRIVER_ARTIFACTS
        ),
    )
    driver = driver.replace(
        "@ORACLE_FAILURE_HELPER@",
        _ORACLE_FAILURE_HELPER
        if oracle_enabled and ir is not None
        else "",
    )
    driver = driver.replace(
        "@ORACLE_FAILURE_CLEANUP@",
        "        Driver_clean_oracle_orphans();\n"
        if oracle_enabled and ir is not None
        else "",
    )
    return {
        "Emitter.h": _render(_EMITTER_H, replacements),
        "Emitter.c": _render(_EMITTER_C, replacements),
        "Driver.c": driver,
    }


_EMITTER_H = r'''/* Generated by Archon. */
#ifndef ARCHON_GENERATED_EMITTER_H
#define ARCHON_GENERATED_EMITTER_H

@ORACLE_INCLUDE@
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum Emitter_Status {
    EMITTER_STATUS_OK = 0,
    EMITTER_STATUS_INVALID_ARGUMENT,
    EMITTER_STATUS_BAD_ARTIFACT,
    EMITTER_STATUS_UNSUPPORTED_DIALECT,
    EMITTER_STATUS_UNKNOWN_INSTRUCTION,
    EMITTER_STATUS_OPERAND_COUNT,
    EMITTER_STATUS_OPERAND_KIND,
    EMITTER_STATUS_OPERAND_RANGE,
    EMITTER_STATUS_INVALID_SYMBOL,
    EMITTER_STATUS_DUPLICATE_LABEL,
    EMITTER_STATUS_UNDEFINED_LABEL,
    EMITTER_STATUS_IO_ERROR,
    EMITTER_STATUS_HOOK_ERROR,
    EMITTER_STATUS_ORACLE_ERROR,
    EMITTER_STATUS_OUT_OF_MEMORY
} Emitter_Status;

typedef enum Emitter_Dialect {
    EMITTER_DIALECT_GAS = 0,
    EMITTER_DIALECT_SANDSTONE = 1
} Emitter_Dialect;

typedef enum Emitter_Operand_Kind {
    EMITTER_OPERAND_REGISTER = 0,
    EMITTER_OPERAND_IMMEDIATE,
    EMITTER_OPERAND_LABEL,
    EMITTER_OPERAND_SYMBOL
} Emitter_Operand_Kind;

typedef enum Emitter_Operand_Role {
    EMITTER_ROLE_INPUT = 0,
    EMITTER_ROLE_OUTPUT,
    EMITTER_ROLE_INOUT
} Emitter_Operand_Role;

typedef struct Emitter_Operand {
    Emitter_Operand_Kind kind;
    int64_t value;
    const char *symbol;
} Emitter_Operand;

typedef struct Emitter_Diagnostic {
    Emitter_Status status;
    unsigned artifact_line;
    char message[256];
} Emitter_Diagnostic;

typedef struct Emitter_Hooks {
    /* begin/instruction/end return zero on success.  format_operand returns
     * the byte count excluding its terminating NUL, or a negative value on
     * failure; Emitter_emit always terminates the accepted buffer. */
    int (*begin)(FILE *output, Emitter_Dialect dialect,
                 const char *target, void *user);
    int (*format_operand)(char *buffer, size_t capacity,
                          Emitter_Dialect dialect,
                          const Emitter_Operand *operand, void *user);
    int (*instruction)(FILE *output, Emitter_Dialect dialect,
                       const char *opcode, const Emitter_Operand *operands,
                       size_t operand_count, void *user);
    int (*end)(FILE *output, Emitter_Dialect dialect,
               const char *target, void *user);
    void *user;
} Emitter_Hooks;

typedef struct Emitter_Options {
    Emitter_Dialect dialect;
    const char *input_path;  /* NULL selects Regallocated.i. */
    const char *output_path; /* NULL selects Program.s or Program.asm. */
    FILE *diagnostics;       /* NULL suppresses automatic diagnostic text. */
    const Emitter_Hooks *hooks;
@ORACLE_OPTION_FIELD@
} Emitter_Options;

typedef struct Emitter_Instruction_Info {
    const char *name;
    const char *identifier;
    const char *primitive;
    size_t operand_count;
    unsigned encoding_bytes;
} Emitter_Instruction_Info;

typedef struct Emitter_Operand_Info {
    const char *name;
    Emitter_Operand_Kind kind;
    Emitter_Operand_Role role;
    unsigned width;
    unsigned is_signed;
} Emitter_Operand_Info;

Emitter_Operand Emitter_register(uint32_t index);
Emitter_Operand Emitter_immediate(int64_t value);
Emitter_Operand Emitter_label(uint32_t id);
Emitter_Operand Emitter_symbol(const char *name);

int Emitter_dialect_supported(Emitter_Dialect dialect);
const char *Emitter_default_output(Emitter_Dialect dialect);
const char *Emitter_status_name(Emitter_Status status);
const char *Emitter_target_name(void);
size_t Emitter_instruction_count(void);
int Emitter_instruction_info(size_t instruction,
                             Emitter_Instruction_Info *info);
int Emitter_operand_info(size_t instruction, size_t operand,
                         Emitter_Operand_Info *info);
int Emitter_select_instruction(const char *primitive, int has_result,
                               const Emitter_Operand_Kind *inputs,
                               size_t input_count, size_t *instruction);
int Emitter_lookup_instruction(const char *name, size_t *instruction);
Emitter_Status Emitter_emit(const Emitter_Options *options,
                            Emitter_Diagnostic *diagnostic);

/*
 * Emitter_emit reads the allocation facts written by Regalloc.c followed by:
 *
 *   REGALLOC_EMISSION_BEGIN(instruction_count)
 *   REGALLOC_LABEL(id)
 *   REGALLOC_INSTRUCTION_BEGIN("OPCODE", operand_count)
 *   REGALLOC_OPERAND_REGISTER(id)
 *   REGALLOC_OPERAND_IMMEDIATE(value)
 *   REGALLOC_OPERAND_LABEL(id)
 *   REGALLOC_OPERAND_SYMBOL("name")
 *   REGALLOC_INSTRUCTION_END()
 *   REGALLOC_EMISSION_END()
 *
 * Every invocation occupies one line.  Unknown or malformed emission records
 * are rejected; ordinary REGALLOC_* allocation facts remain forward-compatible.
 */

#ifdef __cplusplus
}
#endif
#endif
'''


_ORACLE_SUPPORT = r'''
#define EMITTER_ORACLE_TEXT_LIMIT 160u

static int Emitter_oracle_text_valid(const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    size_t length = 0u;
    if (cursor == NULL)
        return 0;
    while (*cursor != 0u) {
        if (*cursor < 0x20u || *cursor > 0x7eu ||
                length == EMITTER_ORACLE_TEXT_LIMIT)
            return 0;
        ++cursor;
        ++length;
    }
    return 1;
}

static Emitter_Status Emitter_oracle_failure(
        Emitter_Context *context, const char *action,
        ArchonOracleStatus oracle_status,
        const ArchonOracleDiagnostic *oracle_diagnostic) {
    const char *detail =
        oracle_diagnostic != NULL && oracle_diagnostic->message[0] != '\0'
        ? oracle_diagnostic->message
        : archon_oracle_status_string(oracle_status);
    return Emitter_fail(
        context, EMITTER_STATUS_ORACLE_ERROR, 0u,
        "%s: %.192s", action, detail);
}

static Emitter_Status Emitter_validate_oracle_annotations(
        Emitter_Context *context,
        const ArchonOracleAnnotations *annotations) {
    size_t index;
    if (annotations->protocol_version != ARCHON_ORACLE_PROTOCOL_VERSION ||
            annotations->stage != ARCHON_ORACLE_STAGE_EMIT)
        return Emitter_fail(
            context, EMITTER_STATUS_ORACLE_ERROR, 0u,
            "Oracle sidecar has the wrong protocol version or stage");
    for (index = 0u; index < annotations->count; ++index) {
        const ArchonOracleAnnotation *entry = &annotations->entries[index];
        if (strcmp(entry->key, "emitter.comment") == 0 ||
                strcmp(entry->key, "emitter.diagnostic") == 0) {
            if (entry->kind != ARCHON_ORACLE_ANNOTATION_STRING ||
                    !Emitter_oracle_text_valid(entry->value.string))
                return Emitter_fail(
                    context, EMITTER_STATUS_ORACLE_ERROR, 0u,
                    "Oracle emitter text metadata is not bounded printable ASCII");
        } else if (strcmp(entry->key, "emitter.dialect") == 0) {
            if (entry->kind != ARCHON_ORACLE_ANNOTATION_STRING ||
                    (strcmp(entry->value.string, "gas") != 0 &&
                     strcmp(entry->value.string, "sandstone") != 0))
                return Emitter_fail(
                    context, EMITTER_STATUS_ORACLE_ERROR, 0u,
                    "Oracle emitter dialect must be gas or sandstone");
        }
    }
    return EMITTER_STATUS_OK;
}

static void Emitter_commit_oracle_annotations(
        Emitter_Context *context,
        const ArchonOracleAnnotations *annotations) {
    size_t index;
    for (index = 0u; index < annotations->count; ++index) {
        const ArchonOracleAnnotation *entry = &annotations->entries[index];
        if (strcmp(entry->key, "emitter.comment") == 0) {
            (void)snprintf(
                context->oracle_comment, sizeof context->oracle_comment,
                "%s", entry->value.string);
        } else if (strcmp(entry->key, "emitter.diagnostic") == 0) {
            (void)snprintf(
                context->oracle_diagnostic,
                sizeof context->oracle_diagnostic,
                "%s", entry->value.string);
        } else if (strcmp(entry->key, "emitter.dialect") == 0) {
            context->dialect = strcmp(entry->value.string, "gas") == 0
                ? EMITTER_DIALECT_GAS : EMITTER_DIALECT_SANDSTONE;
        }
    }
}

static Emitter_Status Emitter_apply_oracle_annotations(
        Emitter_Context *context, const char *artifact_path) {
    ArchonOracleDiagnostic oracle_diagnostic;
    ArchonOracleAnnotations annotations;
    ArchonOracleStatus oracle_status;
    ArchonOracleStatus process_status;
    Emitter_Status status;
    char sidecar[ARCHON_ORACLE_MAX_PATH_BYTES + 1u];
    const char *sidecar_path;
    archon_oracle_diagnostic_init(&oracle_diagnostic);
    process_status = archon_oracle_process_file(
        artifact_path, ARCHON_ORACLE_STAGE_EMIT,
        context->options->oracle, &oracle_diagnostic);
    if (process_status != ARCHON_ORACLE_STATUS_OK &&
            process_status != ARCHON_ORACLE_STATUS_INACTIVE)
        return Emitter_oracle_failure(
            context, "Oracle rejected allocation artifact",
            process_status, &oracle_diagnostic);
    sidecar_path =
        context->options->oracle != NULL &&
        context->options->oracle->annotation_path != NULL &&
        context->options->oracle->annotation_path[0] != '\0'
        ? context->options->oracle->annotation_path : NULL;
    if (sidecar_path == NULL) {
        oracle_status = archon_oracle_default_annotation_path(
            artifact_path, sidecar, sizeof sidecar);
        if (oracle_status != ARCHON_ORACLE_STATUS_OK) {
            if (process_status == ARCHON_ORACLE_STATUS_INACTIVE)
                return EMITTER_STATUS_OK;
            return Emitter_oracle_failure(
                context, "cannot derive Oracle annotation path",
                oracle_status, NULL);
        }
        sidecar_path = sidecar;
    }
    archon_oracle_annotations_init(&annotations);
    archon_oracle_diagnostic_init(&oracle_diagnostic);
    oracle_status = archon_oracle_annotations_load(
        sidecar_path, &annotations, &oracle_diagnostic);
    if (oracle_status == ARCHON_ORACLE_STATUS_NOT_FOUND)
        return EMITTER_STATUS_OK;
    if (oracle_status != ARCHON_ORACLE_STATUS_OK)
        return Emitter_oracle_failure(
            context, "cannot load Oracle annotations",
            oracle_status, &oracle_diagnostic);
    status = Emitter_validate_oracle_annotations(context, &annotations);
    if (status == EMITTER_STATUS_OK)
        Emitter_commit_oracle_annotations(context, &annotations);
    archon_oracle_annotations_dispose(&annotations);
    return status;
}
'''


_EMITTER_C = r'''/* Generated by Archon. */
#include "Emitter.h"
@ORACLE_INCLUDE@

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum Emitter_Meta_Kind {
    EMITTER_META_REGISTER = 0,
    EMITTER_META_IMMEDIATE
} Emitter_Meta_Kind;

typedef struct Emitter_Operand_Metadata {
    const char *name;
    Emitter_Meta_Kind kind;
    Emitter_Operand_Role role;
    unsigned width;
    unsigned is_signed;
} Emitter_Operand_Metadata;

typedef struct Emitter_Instruction_Metadata {
    const char *name;
    const char *identifier;
    const char *primitive;
    const char *gas_template;
    const char *sandstone_template;
    size_t first_operand;
    size_t operand_count;
    unsigned encoding_bytes;
} Emitter_Instruction_Metadata;

typedef struct Emitter_Register_Alias {
    const char *name;
    uint32_t index;
} Emitter_Register_Alias;

static const char Emitter_target[] = @TARGET@;
static const char Emitter_register_prefix[] = @REGISTER_PREFIX@;
enum { Emitter_register_count = @REGISTER_COUNT@ };
static const size_t Emitter_instruction_metadata_count = @INSTRUCTION_COUNT@u;
enum { Emitter_alias_count = @ALIAS_COUNT@ };

static const Emitter_Operand_Metadata Emitter_operand_metadata[] = {
@OPERANDS@
};

static const Emitter_Instruction_Metadata Emitter_instruction_metadata[] = {
@INSTRUCTIONS@
};

static const Emitter_Register_Alias Emitter_register_aliases[] = {
@ALIASES@
};

typedef struct Emitter_Context {
    const Emitter_Options *options;
    Emitter_Diagnostic *diagnostic;
    FILE *output;
    Emitter_Dialect dialect;
    uint32_t *defined_labels;
    size_t defined_count;
    size_t defined_capacity;
    uint32_t *referenced_labels;
    size_t referenced_count;
    size_t referenced_capacity;
    Emitter_Operand operands[32];
    char operand_symbols[32][128];
    size_t operand_count;
    size_t expected_operands;
    size_t expected_instructions;
    size_t emitted_instructions;
    char opcode[128];
    unsigned artifact_line;
    int emission_seen;
    int emission_open;
    int instruction_open;
    int emitted_anything;
    int block_comment_open;
    char oracle_comment[161];
    char oracle_diagnostic[161];
} Emitter_Context;

static Emitter_Status Emitter_fail(Emitter_Context *context,
                                   Emitter_Status status, unsigned line,
                                   const char *format, ...) {
    va_list args;
    Emitter_Diagnostic *diagnostic =
        context != NULL ? context->diagnostic : NULL;
    if (diagnostic != NULL && diagnostic->status == EMITTER_STATUS_OK) {
        diagnostic->status = status;
        diagnostic->artifact_line = line;
        va_start(args, format);
        (void)vsnprintf(diagnostic->message, sizeof diagnostic->message,
                        format, args);
        va_end(args);
        if (context != NULL && context->oracle_diagnostic[0] != '\0') {
            size_t used = strlen(diagnostic->message);
            if (used < sizeof diagnostic->message)
                (void)snprintf(
                    diagnostic->message + used,
                    sizeof diagnostic->message - used,
                    " [Oracle: %.160s]", context->oracle_diagnostic);
        }
    }
    return status;
}

@ORACLE_SUPPORT@

Emitter_Operand Emitter_register(uint32_t index) {
    Emitter_Operand operand;
    operand.kind = EMITTER_OPERAND_REGISTER;
    operand.value = (int64_t)index;
    operand.symbol = NULL;
    return operand;
}

Emitter_Operand Emitter_immediate(int64_t value) {
    Emitter_Operand operand;
    operand.kind = EMITTER_OPERAND_IMMEDIATE;
    operand.value = value;
    operand.symbol = NULL;
    return operand;
}

Emitter_Operand Emitter_label(uint32_t id) {
    Emitter_Operand operand;
    operand.kind = EMITTER_OPERAND_LABEL;
    operand.value = (int64_t)id;
    operand.symbol = NULL;
    return operand;
}

Emitter_Operand Emitter_symbol(const char *name) {
    Emitter_Operand operand;
    operand.kind = EMITTER_OPERAND_SYMBOL;
    operand.value = 0;
    operand.symbol = name;
    return operand;
}

int Emitter_dialect_supported(Emitter_Dialect dialect) {
    if (dialect == EMITTER_DIALECT_GAS)
        return @GAS_SUPPORTED@;
    if (dialect == EMITTER_DIALECT_SANDSTONE)
        return @SANDSTONE_SUPPORTED@;
    return 0;
}

const char *Emitter_default_output(Emitter_Dialect dialect) {
    if (dialect == EMITTER_DIALECT_GAS)
        return "Program.s";
    if (dialect == EMITTER_DIALECT_SANDSTONE)
        return "Program.asm";
    return NULL;
}

const char *Emitter_target_name(void) {
    return Emitter_target;
}

const char *Emitter_status_name(Emitter_Status status) {
    switch (status) {
    case EMITTER_STATUS_OK: return "ok";
    case EMITTER_STATUS_INVALID_ARGUMENT: return "invalid argument";
    case EMITTER_STATUS_BAD_ARTIFACT: return "bad allocation artifact";
    case EMITTER_STATUS_UNSUPPORTED_DIALECT: return "unsupported dialect";
    case EMITTER_STATUS_UNKNOWN_INSTRUCTION: return "unknown instruction";
    case EMITTER_STATUS_OPERAND_COUNT: return "operand count mismatch";
    case EMITTER_STATUS_OPERAND_KIND: return "operand kind mismatch";
    case EMITTER_STATUS_OPERAND_RANGE: return "operand out of range";
    case EMITTER_STATUS_INVALID_SYMBOL: return "invalid symbol";
    case EMITTER_STATUS_DUPLICATE_LABEL: return "duplicate label";
    case EMITTER_STATUS_UNDEFINED_LABEL: return "undefined label";
    case EMITTER_STATUS_IO_ERROR: return "I/O error";
    case EMITTER_STATUS_HOOK_ERROR: return "dialect hook failed";
    case EMITTER_STATUS_ORACLE_ERROR: return "Oracle error";
    case EMITTER_STATUS_OUT_OF_MEMORY: return "out of memory";
    }
    return "unknown emitter status";
}

static int Emitter_name_equal(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (toupper((unsigned char)*left) != toupper((unsigned char)*right))
            return 0;
        ++left;
        ++right;
    }
    return *left == *right;
}

static const Emitter_Instruction_Metadata *
Emitter_find_instruction(const char *name) {
    size_t index;
    if (name == NULL)
        return NULL;
    for (index = 0u; index < Emitter_instruction_metadata_count; ++index)
        if (Emitter_name_equal(name, Emitter_instruction_metadata[index].name))
            return &Emitter_instruction_metadata[index];
    return NULL;
}

size_t Emitter_instruction_count(void) {
    return Emitter_instruction_metadata_count;
}

int Emitter_instruction_info(size_t instruction,
                             Emitter_Instruction_Info *info) {
    const Emitter_Instruction_Metadata *metadata;
    if (info == NULL ||
        instruction >= Emitter_instruction_metadata_count)
        return 0;
    metadata = &Emitter_instruction_metadata[instruction];
    info->name = metadata->name;
    info->identifier = metadata->identifier;
    info->primitive = metadata->primitive;
    info->operand_count = metadata->operand_count;
    info->encoding_bytes = metadata->encoding_bytes;
    return 1;
}

int Emitter_lookup_instruction(const char *name, size_t *instruction) {
    size_t index;
    if (name == NULL || instruction == NULL)
        return 0;
    for (index = 0u;
         index < Emitter_instruction_metadata_count; ++index)
        if (strcmp(name, Emitter_instruction_metadata[index].name) == 0) {
            *instruction = index;
            return 1;
        }
    return 0;
}

int Emitter_operand_info(size_t instruction, size_t operand,
                         Emitter_Operand_Info *info) {
    const Emitter_Instruction_Metadata *metadata;
    const Emitter_Operand_Metadata *row;
    if (info == NULL ||
        instruction >= Emitter_instruction_metadata_count)
        return 0;
    metadata = &Emitter_instruction_metadata[instruction];
    if (operand >= metadata->operand_count)
        return 0;
    row = &Emitter_operand_metadata[metadata->first_operand + operand];
    info->name = row->name;
    info->kind = row->kind == EMITTER_META_REGISTER
        ? EMITTER_OPERAND_REGISTER : EMITTER_OPERAND_IMMEDIATE;
    info->role = row->role;
    info->width = row->width;
    info->is_signed = row->is_signed;
    return 1;
}

int Emitter_select_instruction(const char *primitive, int has_result,
                               const Emitter_Operand_Kind *inputs,
                               size_t input_count, size_t *instruction) {
    size_t candidate;
    size_t best = SIZE_MAX;
    unsigned best_bytes = UINT_MAX;
    if (primitive == NULL || instruction == NULL ||
        (input_count != 0u && inputs == NULL))
        return 0;
    for (candidate = 0u;
         candidate < Emitter_instruction_metadata_count; ++candidate) {
        const Emitter_Instruction_Metadata *metadata =
            &Emitter_instruction_metadata[candidate];
        size_t operand;
        size_t input = 0u;
        unsigned outputs = 0u;
        int valid = metadata->primitive != NULL &&
                    strcmp(metadata->primitive, primitive) == 0;
        for (operand = 0u; valid && operand < metadata->operand_count;
             ++operand) {
            const Emitter_Operand_Metadata *row =
                &Emitter_operand_metadata[metadata->first_operand + operand];
            Emitter_Operand_Kind expected = row->kind == EMITTER_META_REGISTER
                ? EMITTER_OPERAND_REGISTER : EMITTER_OPERAND_IMMEDIATE;
            if (row->role == EMITTER_ROLE_OUTPUT) {
                ++outputs;
            } else if (row->role == EMITTER_ROLE_INPUT) {
                if (input >= input_count || inputs[input] != expected)
                    valid = 0;
                ++input;
            } else {
                /* The ANF bridge has no tied-operand representation. */
                valid = 0;
            }
        }
        if (!valid || outputs != (has_result ? 1u : 0u) ||
            input != input_count)
            continue;
        if (best == SIZE_MAX || metadata->encoding_bytes < best_bytes ||
            (metadata->encoding_bytes == best_bytes &&
             strcmp(metadata->name,
                    Emitter_instruction_metadata[best].name) < 0)) {
            best = candidate;
            best_bytes = metadata->encoding_bytes;
        }
    }
    if (best == SIZE_MAX)
        return 0;
    *instruction = best;
    return 1;
}

static int Emitter_symbol_valid(const char *name) {
    const unsigned char *cursor = (const unsigned char *)name;
    if (cursor == NULL ||
        !(isalpha(*cursor) || *cursor == '_' || *cursor == '.' ||
          *cursor == '$'))
        return 0;
    ++cursor;
    while (*cursor != 0u) {
        if (!(isalnum(*cursor) || *cursor == '_' || *cursor == '.' ||
              *cursor == '$'))
            return 0;
        ++cursor;
    }
    return 1;
}

static Emitter_Status Emitter_track_label(
    Emitter_Context *context, uint32_t id, int definition, unsigned line) {
    uint32_t **items = definition ? &context->defined_labels :
                                    &context->referenced_labels;
    size_t *count = definition ? &context->defined_count :
                                 &context->referenced_count;
    size_t *capacity = definition ? &context->defined_capacity :
                                    &context->referenced_capacity;
    size_t index, next;
    uint32_t *grown;
    for (index = 0u; index < *count; ++index)
        if ((*items)[index] == id)
            return definition
                ? Emitter_fail(context, EMITTER_STATUS_DUPLICATE_LABEL, line,
                               "Regallocated.i defines label %" PRIu32
                               " more than once", id)
                : EMITTER_STATUS_OK;
    if (*count == *capacity) {
        next = *capacity != 0u ? *capacity * 2u : 16u;
        if (next < *capacity || next > SIZE_MAX / sizeof **items)
            return Emitter_fail(context, EMITTER_STATUS_OUT_OF_MEMORY, line,
                                "too many labels");
        grown = (uint32_t *)realloc(*items, next * sizeof **items);
        if (grown == NULL)
            return Emitter_fail(context, EMITTER_STATUS_OUT_OF_MEMORY, line,
                                "cannot allocate label table");
        *items = grown;
        *capacity = next;
    }
    (*items)[(*count)++] = id;
    return EMITTER_STATUS_OK;
}

static Emitter_Status Emitter_write_label(Emitter_Context *context,
                                          uint32_t id, unsigned line) {
    Emitter_Status status = Emitter_track_label(context, id, 1, line);
    if (status != EMITTER_STATUS_OK)
        return status;
    if (fprintf(context->output, ".Larchon_%" PRIu32 ":\n", id) < 0)
        return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, line,
                            "cannot write assembly label");
    context->emitted_anything = 1;
    return EMITTER_STATUS_OK;
}

static int Emitter_signed_fits(int64_t value, unsigned width) {
    int64_t lower, upper;
    if (width == 0u)
        return 0;
    if (width >= 64u)
        return 1;
    lower = -((int64_t)UINT64_C(1) << (width - 1u));
    upper = ((int64_t)UINT64_C(1) << (width - 1u)) - 1;
    return value >= lower && value <= upper;
}

static int Emitter_unsigned_fits(int64_t value, unsigned width) {
    uint64_t upper;
    if (width == 0u || value < 0)
        return 0;
    if (width >= 63u)
        return 1;
    upper = (UINT64_C(1) << width) - 1u;
    return (uint64_t)value <= upper;
}

static Emitter_Status Emitter_validate_operand(
    Emitter_Context *context, const Emitter_Operand_Metadata *metadata,
    const Emitter_Operand *operand, unsigned line) {
    if (metadata->kind == EMITTER_META_REGISTER) {
        uint64_t available_registers =
            (uint64_t)(unsigned)Emitter_register_count;
        if (operand->kind != EMITTER_OPERAND_REGISTER)
            return Emitter_fail(context, EMITTER_STATUS_OPERAND_KIND, line,
                                "operand %s must be a register",
                                metadata->name);
        if (operand->value < 0 ||
            (uint64_t)operand->value >= available_registers)
            return Emitter_fail(context, EMITTER_STATUS_OPERAND_RANGE, line,
                                "register %" PRId64 " is not available",
                                operand->value);
        return EMITTER_STATUS_OK;
    } else if (operand->kind == EMITTER_OPERAND_IMMEDIATE) {
        if (!(metadata->is_signed
                  ? Emitter_signed_fits(operand->value, metadata->width)
                  : Emitter_unsigned_fits(operand->value, metadata->width)))
            return Emitter_fail(context, EMITTER_STATUS_OPERAND_RANGE, line,
                                "immediate %" PRId64
                                " does not fit %s%u",
                                operand->value,
                                 metadata->is_signed ? "sint<" : "uint<",
                                 metadata->width);
        return EMITTER_STATUS_OK;
    } else if (operand->kind == EMITTER_OPERAND_LABEL) {
        uint32_t id;
        if (operand->value < 0 || (uint64_t)operand->value > UINT32_MAX)
            return Emitter_fail(context, EMITTER_STATUS_OPERAND_RANGE, line,
                                "label ID is out of range");
        id = (uint32_t)operand->value;
        {
            Emitter_Status status =
                Emitter_track_label(context, id, 0, line);
            if (status != EMITTER_STATUS_OK)
                return status;
        }
        return EMITTER_STATUS_OK;
    } else if (operand->kind == EMITTER_OPERAND_SYMBOL) {
        if (!Emitter_symbol_valid(operand->symbol))
            return Emitter_fail(context, EMITTER_STATUS_INVALID_SYMBOL, line,
                                "invalid symbol operand");
        return EMITTER_STATUS_OK;
    } else {
        return Emitter_fail(context, EMITTER_STATUS_OPERAND_KIND, line,
                            "operand %s must be an immediate, label, or symbol",
                            metadata->name);
    }
}

static Emitter_Status Emitter_format_operand(
    Emitter_Context *context, const Emitter_Operand_Metadata *metadata,
    const Emitter_Operand *operand, char *buffer, size_t capacity,
    unsigned line) {
    int written;
    if (context->options->hooks != NULL &&
        context->options->hooks->format_operand != NULL) {
        int result = context->options->hooks->format_operand(
            buffer, capacity, context->dialect, operand,
            context->options->hooks->user);
        if (result < 0 || (size_t)result >= capacity)
            return Emitter_fail(context, EMITTER_STATUS_HOOK_ERROR, line,
                                "operand dialect hook failed");
        buffer[result] = '\0';
        return EMITTER_STATUS_OK;
    }
    if (metadata->kind == EMITTER_META_REGISTER)
        written = snprintf(buffer, capacity, "%s%" PRId64,
                           Emitter_register_prefix, operand->value);
    else if (operand->kind == EMITTER_OPERAND_IMMEDIATE)
        written = snprintf(buffer, capacity, "%" PRId64, operand->value);
    else if (operand->kind == EMITTER_OPERAND_LABEL)
        written = snprintf(buffer, capacity, ".Larchon_%" PRIu32,
                           (uint32_t)operand->value);
    else
        written = snprintf(buffer, capacity, "%s", operand->symbol);
    if (written < 0 || (size_t)written >= capacity)
        return Emitter_fail(context, EMITTER_STATUS_OPERAND_RANGE, line,
                            "formatted operand is too long");
    return EMITTER_STATUS_OK;
}

static const char *Emitter_template(
    const Emitter_Instruction_Metadata *instruction,
    Emitter_Dialect dialect) {
    if (dialect == EMITTER_DIALECT_GAS)
        return instruction->gas_template;
    if (dialect == EMITTER_DIALECT_SANDSTONE)
        return instruction->sandstone_template;
    return NULL;
}

static Emitter_Status Emitter_render_template(
    Emitter_Context *context,
    const Emitter_Instruction_Metadata *instruction,
    const Emitter_Operand *operands, unsigned line) {
    const char *template_text =
        Emitter_template(instruction, context->dialect);
    char formatted[32][128];
    size_t index;
    const char *cursor;
    if (template_text == NULL)
        return Emitter_fail(
            context, EMITTER_STATUS_UNSUPPORTED_DIALECT, line,
            "instruction %s has no validated syntax for this dialect",
            instruction->name);
    if (instruction->operand_count > 32u)
        return Emitter_fail(context, EMITTER_STATUS_OPERAND_COUNT, line,
                            "instruction metadata has too many operands");
    for (index = 0u; index < instruction->operand_count; ++index) {
        Emitter_Status status = Emitter_format_operand(
            context,
            &Emitter_operand_metadata[instruction->first_operand + index],
            &operands[index], formatted[index], sizeof formatted[index],
            line);
        if (status != EMITTER_STATUS_OK)
            return status;
    }
    if (fputs("    ", context->output) == EOF)
        return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, line,
                            "cannot write instruction indentation");
    cursor = template_text;
    while (*cursor != '\0') {
        if (*cursor == '{') {
            const char *end = strchr(cursor + 1, '}');
            size_t length;
            int found = 0;
            if (end == NULL)
                return Emitter_fail(context, EMITTER_STATUS_UNSUPPORTED_DIALECT,
                                    line, "invalid assembly template for %s",
                                    instruction->name);
            length = (size_t)(end - cursor - 1);
            for (index = 0u; index < instruction->operand_count; ++index) {
                const char *name =
                    Emitter_operand_metadata[
                        instruction->first_operand + index].name;
                if (strlen(name) == length &&
                    memcmp(name, cursor + 1, length) == 0) {
                    if (fputs(formatted[index], context->output) == EOF)
                        return Emitter_fail(
                            context, EMITTER_STATUS_IO_ERROR, line,
                            "cannot write instruction operand");
                    found = 1;
                    break;
                }
            }
            if (!found)
                return Emitter_fail(context, EMITTER_STATUS_UNSUPPORTED_DIALECT,
                                    line, "unknown placeholder in %s template",
                                    instruction->name);
            cursor = end + 1;
        } else {
            if (fputc((unsigned char)*cursor++, context->output) == EOF)
                return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, line,
                                    "cannot write instruction");
        }
    }
    if (fputc('\n', context->output) == EOF)
        return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, line,
                            "cannot finish instruction");
    return EMITTER_STATUS_OK;
}

static Emitter_Status Emitter_write_instruction(
    Emitter_Context *context, const char *opcode,
    const Emitter_Operand *operands, size_t operand_count, unsigned line) {
    const Emitter_Instruction_Metadata *instruction;
    if (context->diagnostic->status != EMITTER_STATUS_OK)
        return context->diagnostic->status;
    instruction = Emitter_find_instruction(opcode);
    if (instruction == NULL)
        return Emitter_fail(context, EMITTER_STATUS_UNKNOWN_INSTRUCTION, line,
                            "Regallocated.i names unknown instruction %s",
                            opcode != NULL ? opcode : "(null)");
    if (operand_count != instruction->operand_count)
        return Emitter_fail(
            context, EMITTER_STATUS_OPERAND_COUNT, line,
            "instruction %s expects %zu operands, got %zu",
            instruction->name, instruction->operand_count, operand_count);
    {
        size_t operand;
        for (operand = 0u; operand < operand_count; ++operand) {
            Emitter_Status status = Emitter_validate_operand(
                context,
                &Emitter_operand_metadata[
                    instruction->first_operand + operand],
                &operands[operand], line);
            if (status != EMITTER_STATUS_OK)
                return status;
        }
    }
    context->emitted_anything = 1;
    if (context->options->hooks != NULL &&
        context->options->hooks->instruction != NULL) {
        if (context->options->hooks->instruction(
                context->output, context->dialect, instruction->name,
                operands, operand_count,
                context->options->hooks->user) != 0)
            return Emitter_fail(context, EMITTER_STATUS_HOOK_ERROR, line,
                                "instruction dialect hook failed for %s",
                                instruction->name);
        return EMITTER_STATUS_OK;
    }
    return Emitter_render_template(context, instruction, operands, line);
}

static Emitter_Status Emitter_write_prelude(Emitter_Context *context) {
    const Emitter_Hooks *hooks = context->options->hooks;
    (void)Emitter_alias_count;
    (void)Emitter_register_aliases;
    if (hooks != NULL && hooks->begin != NULL) {
        if (hooks->begin(context->output, context->dialect,
                         Emitter_target, hooks->user) != 0)
            return Emitter_fail(context, EMITTER_STATUS_HOOK_ERROR, 0u,
                                "dialect begin hook failed");
        return EMITTER_STATUS_OK;
    }
    if (!Emitter_dialect_supported(context->dialect) &&
        (hooks == NULL || hooks->instruction == NULL))
        return Emitter_fail(
            context, EMITTER_STATUS_UNSUPPORTED_DIALECT, 0u,
            "target %s has no validated built-in syntax for this dialect",
            Emitter_target);
    if (context->dialect == EMITTER_DIALECT_GAS) {
        if (fprintf(context->output,
                    "# Generated by Archon for %s.\n.text\n", Emitter_target) < 0)
            return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, 0u,
                                "cannot write GAS prelude");
    } else if (context->dialect == EMITTER_DIALECT_SANDSTONE) {
        /*
         * SandAS Lua executes before target assembly.  The check is conditional
         * so an ArchML-only invocation (which exposes no SymAS targets) remains
         * valid, while a loaded mismatching SymAS bank fails before assembly.
         */
        static const char *const names[] = {@TARGET_LUA_NAMES@};
        size_t index;
        if (fprintf(context->output,
                    "; Generated by Archon for %s.\n"
                    "<:\n"
                    "local targets = lsandas.symas.targets\n"
                    "local selected = nil\n",
                    Emitter_target) < 0)
            return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, 0u,
                                "cannot write Sandstone prelude");
        for (index = 0u; index < sizeof names / sizeof names[0]; ++index)
            if (fprintf(context->output,
                        "selected = selected or targets[%c%s%c]\n",
                        '"', names[index], '"') < 0)
                return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, 0u,
                                    "cannot write Sandstone target check");
        if (fprintf(context->output,
                    "if next(targets) ~= nil and selected == nil then\n"
                    "  error(%cArchon output target does not match loaded "
                    "Sandstone metadata%c)\n"
                    "end\n"
                    ":>\n"
                    ".text\n",
                    '"', '"') < 0)
            return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, 0u,
                                "cannot finish Sandstone prelude");
    } else {
        return Emitter_fail(context, EMITTER_STATUS_UNSUPPORTED_DIALECT, 0u,
                            "unknown assembly dialect");
    }
@ORACLE_COMMENT@
    return EMITTER_STATUS_OK;
}

static Emitter_Status Emitter_finish(Emitter_Context *context) {
    size_t reference, definition;
    const Emitter_Hooks *hooks = context->options->hooks;
    for (reference = 0u; reference < context->referenced_count; ++reference) {
        int found = 0;
        for (definition = 0u; definition < context->defined_count; ++definition)
            if (context->referenced_labels[reference] ==
                context->defined_labels[definition]) {
                found = 1;
                break;
            }
        if (!found)
            return Emitter_fail(
                context, EMITTER_STATUS_UNDEFINED_LABEL, 0u,
                "label %" PRIu32 " is referenced but not defined",
                context->referenced_labels[reference]);
    }
    if (hooks != NULL && hooks->end != NULL &&
        hooks->end(context->output, context->dialect,
                   Emitter_target, hooks->user) != 0)
        return Emitter_fail(context, EMITTER_STATUS_HOOK_ERROR, 0u,
                            "dialect end hook failed");
    if (fflush(context->output) != 0 || ferror(context->output))
        return Emitter_fail(context, EMITTER_STATUS_IO_ERROR, 0u,
                            "cannot flush assembly output");
    return EMITTER_STATUS_OK;
}

static char *Emitter_trim(char *text) {
    char *end;
    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end != text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

static void Emitter_strip_comments(Emitter_Context *context, char *line) {
    char *read = line;
    char *write = line;
    int quoted = 0;
    int escaped = 0;
    while (*read != '\0') {
        if (context->block_comment_open) {
            if (read[0] == '*' && read[1] == '/') {
                context->block_comment_open = 0;
                read += 2;
            } else {
                ++read;
            }
            continue;
        }
        if (!quoted && read[0] == '/' && read[1] == '/')
            break;
        if (!quoted && read[0] == '/' && read[1] == '*') {
            if (write != line && !isspace((unsigned char)write[-1]))
                *write++ = ' ';
            context->block_comment_open = 1;
            read += 2;
            continue;
        }
        if (*read == '"' && !escaped)
            quoted = !quoted;
        *write++ = *read;
        if (quoted && *read == '\\' && !escaped)
            escaped = 1;
        else
            escaped = 0;
        ++read;
    }
    *write = '\0';
}

static int Emitter_invocation(char *line, char **name, char **arguments,
                              size_t *count, size_t maximum) {
    char *open, *close, *cursor, *begin, *tail;
    int quoted = 0;
    int escaped = 0;
    unsigned depth = 0u;
    line = Emitter_trim(line);
    if (*line == '\0' || *line == '#' ||
        (line[0] == '/' && (line[1] == '/' || line[1] == '*')))
        return 0;
    open = strchr(line, '(');
    close = strrchr(line, ')');
    if (open == NULL || close == NULL || close < open)
        return -1;
    tail = Emitter_trim(close + 1);
    if (*tail == ';')
        tail = Emitter_trim(tail + 1);
    if (*tail != '\0')
        return -1;
    *open = '\0';
    *close = '\0';
    *name = Emitter_trim(line);
    if (**name == '\0')
        return -1;
    *count = 0u;
    begin = Emitter_trim(open + 1);
    if (*begin == '\0')
        return 1;
    cursor = begin;
    for (;;) {
        int ch = (unsigned char)*cursor;
        if (ch == '\0' || (ch == ',' && !quoted && depth == 0u)) {
            if (*count >= maximum)
                return -1;
            *cursor = '\0';
            arguments[(*count)++] = Emitter_trim(begin);
            if (*arguments[*count - 1u] == '\0')
                return -1;
            if (ch == '\0')
                break;
            begin = cursor + 1;
        } else if (ch == '"' && !escaped) {
            quoted = !quoted;
        } else if (!quoted && ch == '(') {
            ++depth;
        } else if (!quoted && ch == ')') {
            if (depth == 0u)
                return -1;
            --depth;
        }
        if (ch == '\\' && !escaped)
            escaped = 1;
        else
            escaped = 0;
        ++cursor;
    }
    return quoted || depth != 0u ? -1 : 1;
}

static int Emitter_u32(const char *text, uint32_t *value) {
    char *end;
    uintmax_t parsed;
    errno = 0;
    while (isspace((unsigned char)*text))
        ++text;
    parsed = strtoumax(text, &end, 0);
    while (isspace((unsigned char)*end))
        ++end;
    if (*text == '\0' || *end != '\0' || errno == ERANGE ||
        parsed > UINT32_MAX)
        return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static int Emitter_size(const char *text, size_t *value) {
    uint32_t parsed;
    if (!Emitter_u32(text, &parsed))
        return 0;
    *value = (size_t)parsed;
    return 1;
}

static int Emitter_i64(const char *text, int64_t *value) {
    char *end;
    intmax_t parsed;
    errno = 0;
    while (isspace((unsigned char)*text))
        ++text;
    parsed = strtoimax(text, &end, 0);
    while (isspace((unsigned char)*end))
        ++end;
    if (*text == '\0' || *end != '\0' || errno == ERANGE ||
        parsed < INT64_MIN || parsed > INT64_MAX)
        return 0;
    *value = (int64_t)parsed;
    return 1;
}

static int Emitter_unquote(char *text) {
    size_t read, write, length = strlen(text);
    if (length < 2u || text[0] != '"' || text[length - 1u] != '"')
        return 0;
    for (read = 1u, write = 0u; read + 1u < length; ++read) {
        unsigned char ch = (unsigned char)text[read];
        if (ch == '\\') {
            unsigned value = 0u;
            size_t digits = 0u;
            if (++read + 1u >= length)
                return 0;
            ch = (unsigned char)text[read];
            if (ch >= '0' && ch <= '7') {
                do {
                    value = value * 8u + (unsigned)(text[read] - '0');
                    ++digits;
                    ++read;
                } while (read + 1u < length && digits < 3u &&
                         text[read] >= '0' && text[read] <= '7');
                --read;
                if (value == 0u || value > UCHAR_MAX)
                    return 0;
                ch = (unsigned char)value;
            } else if (ch != '"' && ch != '\\') {
                return 0;
            }
        }
        text[write++] = (char)ch;
    }
    text[write] = '\0';
    return 1;
}

static Emitter_Status Emitter_bad_artifact(
        Emitter_Context *context, const char *message) {
    return Emitter_fail(context, EMITTER_STATUS_BAD_ARTIFACT,
                        context->artifact_line, "%s", message);
}

static Emitter_Status Emitter_add_artifact_operand(
        Emitter_Context *context, Emitter_Operand operand) {
    if (!context->emission_open || !context->instruction_open)
        return Emitter_bad_artifact(
            context, "operand appears outside an instruction");
    if (context->operand_count >=
        sizeof context->operands / sizeof context->operands[0])
        return Emitter_bad_artifact(context, "instruction has too many operands");
    context->operands[context->operand_count++] = operand;
    return EMITTER_STATUS_OK;
}

static Emitter_Status Emitter_artifact_record(
        Emitter_Context *context, char *name, char **arguments, size_t count) {
    uint32_t id;
    int64_t immediate;
    size_t expected;
    if (strcmp(name, "REGALLOC_EMISSION_BEGIN") == 0) {
        if (count != 1u || context->emission_seen ||
            context->emission_open ||
            !Emitter_size(arguments[0], &expected))
            return Emitter_bad_artifact(context, "invalid emission begin");
        context->emission_open = 1;
        context->emission_seen = 1;
        context->expected_instructions = expected;
        return EMITTER_STATUS_OK;
    }
    if (strcmp(name, "REGALLOC_EMISSION_END") == 0) {
        if (count != 0u || !context->emission_open ||
            context->instruction_open ||
            context->emitted_instructions != context->expected_instructions)
            return Emitter_bad_artifact(context, "invalid emission end");
        context->emission_open = 0;
        return EMITTER_STATUS_OK;
    }
    if (strcmp(name, "REGALLOC_LABEL") == 0) {
        if (count != 1u || !context->emission_open ||
            context->instruction_open || !Emitter_u32(arguments[0], &id))
            return Emitter_bad_artifact(context, "invalid label record");
        return Emitter_write_label(context, id, context->artifact_line);
    }
    if (strcmp(name, "REGALLOC_INSTRUCTION_BEGIN") == 0) {
        size_t length;
        if (count != 2u || !context->emission_open ||
            context->instruction_open || !Emitter_unquote(arguments[0]) ||
            !Emitter_size(arguments[1], &expected))
            return Emitter_bad_artifact(context, "invalid instruction begin");
        length = strlen(arguments[0]);
        if (length == 0u || length >= sizeof context->opcode ||
            expected > sizeof context->operands / sizeof context->operands[0])
            return Emitter_bad_artifact(context, "invalid instruction header");
        memcpy(context->opcode, arguments[0], length + 1u);
        context->expected_operands = expected;
        context->operand_count = 0u;
        context->instruction_open = 1;
        return EMITTER_STATUS_OK;
    }
    if (strcmp(name, "REGALLOC_INSTRUCTION_END") == 0) {
        Emitter_Status status;
        if (count != 0u || !context->instruction_open ||
            context->operand_count != context->expected_operands)
            return Emitter_bad_artifact(context, "invalid instruction end");
        status = Emitter_write_instruction(
            context, context->opcode, context->operands,
            context->operand_count, context->artifact_line);
        if (status != EMITTER_STATUS_OK)
            return status;
        context->instruction_open = 0;
        ++context->emitted_instructions;
        return EMITTER_STATUS_OK;
    }
    if (strcmp(name, "REGALLOC_OPERAND_REGISTER") == 0) {
        if (count != 1u || !Emitter_u32(arguments[0], &id))
            return Emitter_bad_artifact(context, "invalid register operand");
        return Emitter_add_artifact_operand(context, Emitter_register(id));
    }
    if (strcmp(name, "REGALLOC_OPERAND_IMMEDIATE") == 0) {
        if (count != 1u || !Emitter_i64(arguments[0], &immediate))
            return Emitter_bad_artifact(context, "invalid immediate operand");
        return Emitter_add_artifact_operand(context, Emitter_immediate(immediate));
    }
    if (strcmp(name, "REGALLOC_OPERAND_LABEL") == 0) {
        if (count != 1u || !Emitter_u32(arguments[0], &id))
            return Emitter_bad_artifact(context, "invalid label operand");
        return Emitter_add_artifact_operand(context, Emitter_label(id));
    }
    if (strcmp(name, "REGALLOC_OPERAND_SYMBOL") == 0) {
        size_t length;
        if (count != 1u || !Emitter_unquote(arguments[0]))
            return Emitter_bad_artifact(context, "invalid symbol operand");
        length = strlen(arguments[0]);
        if (context->operand_count >=
                sizeof context->operand_symbols /
                    sizeof context->operand_symbols[0] ||
            length >= sizeof context->operand_symbols[0])
            return Emitter_bad_artifact(context, "symbol operand is too long");
        memcpy(context->operand_symbols[context->operand_count],
               arguments[0], length + 1u);
        return Emitter_add_artifact_operand(
            context,
            Emitter_symbol(
                context->operand_symbols[context->operand_count]));
    }
    if (!context->emission_open &&
        (strncmp(name, "REGALLOC_", 9u) == 0 ||
         strncmp(name, "ARCHON_REGALLOCATED_", 20u) == 0))
        return EMITTER_STATUS_OK;
    return Emitter_bad_artifact(context, "unknown allocation artifact record");
}

static Emitter_Status Emitter_consume_artifact(
        Emitter_Context *context, FILE *artifact) {
    char line[4096];
    while (fgets(line, sizeof line, artifact) != NULL) {
        char *name;
        char *arguments[16];
        size_t count;
        int parsed;
        ++context->artifact_line;
        if (strchr(line, '\n') == NULL && !feof(artifact))
            return Emitter_bad_artifact(context, "artifact line is too long");
        Emitter_strip_comments(context, line);
        parsed = Emitter_invocation(
            line, &name, arguments, &count,
            sizeof arguments / sizeof arguments[0]);
        if (parsed < 0)
            return Emitter_bad_artifact(
                context, "malformed macro invocation");
        if (parsed > 0) {
            Emitter_Status status =
                Emitter_artifact_record(context, name, arguments, count);
            if (status != EMITTER_STATUS_OK)
                return status;
        }
    }
    if (ferror(artifact))
        return Emitter_fail(context, EMITTER_STATUS_IO_ERROR,
                            context->artifact_line,
                            "cannot read Regallocated.i");
    if (context->block_comment_open)
        return Emitter_bad_artifact(
            context, "allocation artifact has an unterminated comment");
    if (!context->emission_seen || context->emission_open ||
        context->instruction_open ||
        context->emitted_instructions != context->expected_instructions)
        return Emitter_bad_artifact(
            context, "allocation artifact has no complete emission section");
    return EMITTER_STATUS_OK;
}

Emitter_Status Emitter_emit(const Emitter_Options *options,
                            Emitter_Diagnostic *diagnostic) {
    Emitter_Diagnostic local_diagnostic;
    Emitter_Context context;
    Emitter_Status status = EMITTER_STATUS_OK;
    const char *input_path;
    const char *output_path = NULL;
    FILE *artifact = NULL;
    int output_created = 0;
    memset(&local_diagnostic, 0, sizeof local_diagnostic);
    if (diagnostic == NULL)
        diagnostic = &local_diagnostic;
    memset(diagnostic, 0, sizeof *diagnostic);
    memset(&context, 0, sizeof context);
    context.options = options;
    context.diagnostic = diagnostic;
    if (options == NULL ||
        (options->dialect != EMITTER_DIALECT_GAS &&
         options->dialect != EMITTER_DIALECT_SANDSTONE))
        return Emitter_fail(&context, EMITTER_STATUS_INVALID_ARGUMENT, 0u,
                            "invalid emitter options");
    context.dialect = options->dialect;
    input_path = options->input_path != NULL
        ? options->input_path : "Regallocated.i";
    if (input_path[0] == '\0')
        return Emitter_fail(&context, EMITTER_STATUS_INVALID_ARGUMENT, 0u,
                            "invalid input path");
@ORACLE_PREPARE@
    /*
     * Oracle commits by atomic rename.  Open only after it returns so the
     * transformed artifact is consumed by the ordinary strict parser and
     * operand validator rather than bypassing either one.
     */
    artifact = fopen(input_path, "rb");
    if (artifact == NULL) {
        status = Emitter_fail(
            &context, EMITTER_STATUS_IO_ERROR, 0u,
            "cannot open allocation artifact %s", input_path);
        goto done;
    }
    output_path = options->output_path != NULL
        ? options->output_path : Emitter_default_output(context.dialect);
    if (output_path == NULL || output_path[0] == '\0' ||
            strcmp(input_path, output_path) == 0) {
        status = Emitter_fail(
            &context, EMITTER_STATUS_INVALID_ARGUMENT, 0u,
            "invalid output path");
        goto done;
    }
    context.output = fopen(output_path, "wb");
    if (context.output == NULL) {
        status = Emitter_fail(&context, EMITTER_STATUS_IO_ERROR, 0u,
                              "cannot open output file %s", output_path);
    } else {
        output_created = 1;
        status = Emitter_write_prelude(&context);
    }
    if (status == EMITTER_STATUS_OK)
        status = Emitter_consume_artifact(&context, artifact);
    if (status == EMITTER_STATUS_OK)
        status = Emitter_finish(&context);

done:
    if (artifact != NULL && fclose(artifact) != 0 &&
            status == EMITTER_STATUS_OK)
        status = Emitter_fail(&context, EMITTER_STATUS_IO_ERROR, 0u,
                              "cannot close allocation artifact %s",
                              input_path);
    if (context.output != NULL && fclose(context.output) != 0 &&
        status == EMITTER_STATUS_OK)
        status = Emitter_fail(&context, EMITTER_STATUS_IO_ERROR, 0u,
                              "cannot close output file %s", output_path);
    free(context.defined_labels);
    free(context.referenced_labels);
    if (status != EMITTER_STATUS_OK && output_created)
        (void)remove(output_path);
    if (status != EMITTER_STATUS_OK && options->diagnostics != NULL) {
        if (diagnostic->artifact_line != 0u)
            (void)fprintf(options->diagnostics,
                          "%s:%u: emitter: %s\n", input_path,
                          diagnostic->artifact_line, diagnostic->message);
        else
            (void)fprintf(options->diagnostics, "emitter: %s\n",
                          diagnostic->message);
    }
    return status;
}
'''


_EMITTER_ONLY_DRIVER_C = r'''/* Generated by Archon: emitter-only driver. */
#include "Emitter.h"
@ORACLE_INCLUDE@

#include <stdio.h>
#include <string.h>

static void Driver_usage(FILE *output, const char *program) {
    (void)fprintf(
        output,
        "usage: %s --help\n"
        "This emitter-only stage has no source-IR pipeline.  Generate a full "
        "IR tree or call Emitter_emit with an existing Regallocated.i.\n",
        program);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        Driver_usage(stdout, argv[0]);
        return 0;
    }
    (void)argc;
    (void)argv;
    (void)fprintf(
        stderr,
        "driver: full pipeline unavailable in an emitter-only generation\n");
    return 1;
}
'''


_DRIVER_ARTIFACTS = r'''    "Program.i",
    "Rewritten.i",
    "MIR.i",
    "Selected.i",
    "InstrMap.i",
    "Scheduled.i",
    "RegMap.i",
    "Regallocated.i",
    "Program.s",
    "Program.asm"
'''


_ORACLE_DRIVER_ARTIFACTS = r'''    "Program.i",
    "Rewritten.i",
    "Rewritten.oracle.i",
    "MIR.i",
    "MIR.oracle.i",
    "Selected.i",
    "Selected.oracle.i",
    "InstrMap.i",
    "InstrMap.oracle.i",
    "Scheduled.i",
    "Scheduled.oracle.i",
    "RegMap.i",
    "RegMap.oracle.i",
    "Regallocated.i",
    "Regallocated.oracle.i",
    "Program.s",
    "Program.asm"
'''


_ORACLE_FAILURE_HELPER = r'''
static void Driver_clean_oracle_orphans(void) {
    static const struct {
        const char *artifact;
        const char *sidecar;
    } pairs[] = {
        {"Rewritten.i", "Rewritten.oracle.i"},
        {"MIR.i", "MIR.oracle.i"},
        {"Selected.i", "Selected.oracle.i"},
        {"InstrMap.i", "InstrMap.oracle.i"},
        {"Scheduled.i", "Scheduled.oracle.i"},
        {"RegMap.i", "RegMap.oracle.i"},
        {"Regallocated.i", "Regallocated.oracle.i"}
    };
    size_t index;
    for (index = 0u; index < sizeof pairs / sizeof pairs[0]; ++index) {
        if (!Driver_exists(pairs[index].artifact) &&
                remove(pairs[index].sidecar) != 0 &&
                errno != ENOENT)
            (void)fprintf(
                stderr, "driver: cannot remove %s: %s\n",
                pairs[index].sidecar, strerror(errno));
    }
}
'''


_UNIVERSAL_DRIVER_C = r'''/* Generated by Archon: universal shell-free driver. */
#include "Pipeline.h"
@ORACLE_INCLUDE@

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const Driver_stage_names[] = {
    "decode", "rewrite", "schedule", "isel", "isched", "regalloc", "emit"
};

static const char *const Driver_artifacts[] = {
@DRIVER_ARTIFACTS@
};

static int Driver_stage(
        const char *text, ArchonPipelineStage *stage) {
    size_t index;
    if (text == NULL || stage == NULL)
        return 0;
    for (index = 0u;
         index < sizeof Driver_stage_names /
                     sizeof Driver_stage_names[0];
         ++index)
        if (strcmp(text, Driver_stage_names[index]) == 0) {
            *stage = (ArchonPipelineStage)index;
            return 1;
        }
    return 0;
}

static void Driver_usage(FILE *output, const char *program) {
    (void)fprintf(
        output,
        "usage: %s [--dialect gas|sandstone] [--output FILE]\n"
        "          [--rewrite conventional|egraph]\n"
        "          [--stop-after STAGE] [--keep-artifacts]\n"
        "       %s --list-artifacts\n"
        "       %s --clean\n",
        program, program, program);
}

static int Driver_exists(const char *path) {
    FILE *input = fopen(path, "rb");
    if (input == NULL)
        return 0;
    (void)fclose(input);
    return 1;
}

static int Driver_clean(int include_outputs) {
    size_t index;
    int failed = 0;
    size_t count = sizeof Driver_artifacts / sizeof Driver_artifacts[0];
    /* Program.i is pipeline input, not disposable generated state. */
    for (index = 1u; index < count; ++index) {
        if (!include_outputs && index + 2u >= count)
            continue;
        if (remove(Driver_artifacts[index]) != 0 && errno != ENOENT) {
            (void)fprintf(
                stderr, "driver: cannot remove %s: %s\n",
                Driver_artifacts[index], strerror(errno));
            failed = 1;
        }
    }
    return failed;
}

@ORACLE_FAILURE_HELPER@

int main(int argc, char **argv) {
    Emitter_Options emitter;
    ArchonPipelineOptions pipeline;
@ORACLE_DRIVER_LOCAL@
    int keep = 0;
    int index;
    memset(&emitter, 0, sizeof emitter);
    memset(&pipeline, 0, sizeof pipeline);
    emitter.dialect = EMITTER_DIALECT_GAS;
    emitter.diagnostics = stderr;
    pipeline.stop_after = ARCHON_PIPELINE_EMIT;
    pipeline.rewrite = ARCHON_PIPELINE_REWRITE_CONVENTIONAL;
    pipeline.emitter = &emitter;
    pipeline.diagnostics = stderr;
@ORACLE_DRIVER_INIT@

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--list-artifacts") == 0) {
            size_t artifact;
            for (artifact = 0u;
                 artifact < sizeof Driver_artifacts /
                                sizeof Driver_artifacts[0];
                 ++artifact)
                (void)puts(Driver_artifacts[artifact]);
            return 0;
        }
        if (strcmp(argv[index], "--clean") == 0)
            return Driver_clean(1);
        if (strcmp(argv[index], "--keep-artifacts") == 0) {
            keep = 1;
            continue;
        }
        if (strcmp(argv[index], "--dialect") == 0) {
            if (++index == argc) {
                Driver_usage(stderr, argv[0]);
                return 2;
            }
            if (strcmp(argv[index], "gas") == 0)
                emitter.dialect = EMITTER_DIALECT_GAS;
            else if (strcmp(argv[index], "sandstone") == 0)
                emitter.dialect = EMITTER_DIALECT_SANDSTONE;
            else {
                (void)fprintf(
                    stderr, "driver: unknown dialect %s\n", argv[index]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[index], "--rewrite") == 0) {
            if (++index == argc) {
                Driver_usage(stderr, argv[0]);
                return 2;
            }
            if (strcmp(argv[index], "conventional") == 0)
                pipeline.rewrite =
                    ARCHON_PIPELINE_REWRITE_CONVENTIONAL;
            else if (strcmp(argv[index], "egraph") == 0)
                pipeline.rewrite = ARCHON_PIPELINE_REWRITE_EGRAPH;
            else {
                (void)fprintf(
                    stderr, "driver: unknown rewrite engine %s\n",
                    argv[index]);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[index], "--output") == 0) {
            if (++index == argc || argv[index][0] == '\0') {
                Driver_usage(stderr, argv[0]);
                return 2;
            }
            emitter.output_path = argv[index];
            continue;
        }
        if (strcmp(argv[index], "--stop-after") == 0) {
            if (++index == argc ||
                    !Driver_stage(argv[index], &pipeline.stop_after)) {
                (void)fprintf(
                    stderr, "driver: invalid --stop-after stage\n");
                return 2;
            }
            continue;
        }
        if (strcmp(argv[index], "--help") == 0) {
            Driver_usage(stdout, argv[0]);
            return 0;
        }
        (void)fprintf(
            stderr, "driver: unexpected argument %s\n", argv[index]);
        Driver_usage(stderr, argv[0]);
        return 2;
    }

    /*
     * Emission from a deliberately retained allocation artifact is a valid
     * restart point.  Source input always wins so stale intermediates cannot
     * bypass decoding or validation.
     */
    if (pipeline.stop_after == ARCHON_PIPELINE_EMIT &&
            !Driver_exists("Program.i") &&
            Driver_exists("Regallocated.i")) {
        Emitter_Diagnostic diagnostic;
        if (Emitter_emit(&emitter, &diagnostic) != EMITTER_STATUS_OK)
            return 1;
        return keep ? 0 : Driver_clean(0);
    }

    if (Driver_clean(0) != 0)
        return 1;
    if (archon_pipeline_run(&pipeline) != 0) {
        if (!keep)
            (void)Driver_clean(0);
@ORACLE_FAILURE_CLEANUP@
        return 1;
    }
    if (!keep && pipeline.stop_after == ARCHON_PIPELINE_EMIT)
        return Driver_clean(0);
    return 0;
}
'''
