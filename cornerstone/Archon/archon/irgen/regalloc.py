"""Generate a deterministic C99 linear-scan register allocator.

The generated tool consumes the textual X-macro streams ``Scheduled.i``,
``MIR.i``, and ``RegMap.i``.  Reading them as data (rather than preprocessing
them into the allocator) makes diagnostics source/line aware and lets an
ArchDB-generated register map augment the defaults compiled into the tool.
"""

from __future__ import annotations

from collections.abc import Mapping

from ..diagnostics import ArchonError


NAME = "linear-scan"
ALIASES = ("regalloc", "linear_scan", "linear-scan-regalloc", "lsra")


def _oracle_enabled(context: object) -> bool:
    if not isinstance(context, Mapping):
        return True
    options = context.get("options", {})
    if options is None:
        options = {}
    if not isinstance(options, Mapping):
        raise ArchonError("register-allocation options must be an object")
    enabled = options.get("oracle", True)
    if type(enabled) is not bool:
        raise ArchonError("register-allocation option 'oracle' must be boolean")
    return enabled


def _database(context) -> Mapping:
    if context is None:
        return {}
    if isinstance(context, Mapping):
        for key in ("db", "archdb", "database"):
            value = context.get(key)
            if isinstance(value, Mapping):
                return value
        for key in (
            "architectures",
            "databases",
            "archdbs",
            "selected_databases",
            "selected_archdbs",
            "targets",
        ):
            value = context.get(key)
            if isinstance(value, (list, tuple)) and value:
                first = value[0]
                if isinstance(first, Mapping):
                    for nested in ("db", "archdb", "database"):
                        candidate = first.get(nested)
                        if isinstance(candidate, Mapping):
                            first = candidate
                            break
                    if "registers" in first:
                        return first
        if "registers" in context:
            return context
    for attribute in ("db", "archdb", "database"):
        value = getattr(context, attribute, None)
        if isinstance(value, Mapping):
            return value
    return {}


def _c_string(value: str) -> str:
    data = value.encode("utf-8")
    pieces = ['"']
    for byte in data:
        if byte == 0x22:
            pieces.append(r"\"")
        elif byte == 0x5C:
            pieces.append(r"\\")
        elif 0x20 <= byte <= 0x7E and byte not in (0x3F, 0x40):
            pieces.append(chr(byte))
        else:
            pieces.append(f"\\{byte:03o}")
    pieces.append('"')
    return "".join(pieces)


def _target_rows(db: Mapping) -> tuple[str, str, str]:
    registers = db.get("registers")
    if not isinstance(registers, Mapping):
        return (
            '    {UINT32_MAX, "", 0u, 0u},',
            '    {"", UINT32_MAX},',
            "0u, 0u",
        )
    count = registers.get("count", 0)
    prefix = registers.get("prefix", "r")
    if type(count) is not int or count < 0:
        count = 0
    if not isinstance(prefix, str):
        prefix = "r"
    zero = registers.get("zero")
    reserved: set[int] = set()
    if type(zero) is int and 0 <= zero < count:
        reserved.add(zero)
    for source in (
        registers.get("reserved", ()),
        db.get("reserved_registers", ()),
        db.get("abi", {}).get("reserved", ()) if isinstance(db.get("abi"), Mapping) else (),
    ):
        if not isinstance(source, (list, tuple, set, frozenset)):
            continue
        for item in source:
            if type(item) is int and 0 <= item < count:
                reserved.add(item)
            elif isinstance(item, str):
                aliases = registers.get("aliases", {})
                if isinstance(aliases, Mapping) and type(aliases.get(item)) is int:
                    index = aliases[item]
                    if 0 <= index < count:
                        reserved.add(index)
    register_rows = []
    for index in range(count):
        register_rows.append(
            f"    {{{index}u, {_c_string(f'{prefix}{index}')}, "
            f"{int(index in reserved)}u, {int(index == zero)}u}},"
        )
    aliases = registers.get("aliases", {})
    alias_rows = []
    if isinstance(aliases, Mapping):
        for name, index in sorted(aliases.items(), key=lambda item: str(item[0])):
            if isinstance(name, str) and type(index) is int and 0 <= index < count:
                alias_rows.append(f"    {{{_c_string(name)}, {index}u}},")
    return (
        "\n".join(register_rows) or '    {UINT32_MAX, "", 0u, 0u},',
        "\n".join(alias_rows) or '    {"", UINT32_MAX},',
        f"{len(register_rows)}u, {len(alias_rows)}u",
    )


def emit(context=None) -> dict[str, str]:
    """Emit ``Regalloc.h`` and ``Regalloc.c`` for *context*'s ArchDB."""
    register_rows, alias_rows, counts = _target_rows(_database(context))
    register_count, alias_count = counts.split(", ")
    oracle_enabled = _oracle_enabled(context)
    oracle_include = '#include "Oracle.h"\n' if oracle_enabled else ""
    oracle_support = _ORACLE_SUPPORT if oracle_enabled else ""
    oracle_allocate = (
        "    if (!allocator->oracle_annotations_applied) {\n"
        "        status = RA_apply_oracle_annotations(\n"
        '            allocator, "RegMap.i", diagnostic);\n'
        "        if (status != RA_OK)\n"
        "            return status;\n"
        "    }\n"
        if oracle_enabled
        else ""
    )
    oracle_load_regmap = (
        "    status = RA_OK;\n"
        "    if (regmap_path != NULL)\n"
        "        status = RA_process_oracle_artifact(\n"
        "            regmap_path, diagnostic);\n"
        "    if (status == RA_OK)\n"
        "        status = RA_open_and_load(\n"
        "            &allocator, regmap_path,\n"
        "            RA_STREAM_REGMAP, diagnostic);\n"
        if oracle_enabled
        else (
            "    status = RA_open_and_load(\n"
            "        &allocator, regmap_path,\n"
            "        RA_STREAM_REGMAP, diagnostic);\n"
        )
    )
    oracle_run_files = (
        "    if (status == RA_OK)\n"
        "        status = RA_apply_oracle_annotations(\n"
        "            &allocator, regmap_path, diagnostic);\n"
        if oracle_enabled
        else ""
    )
    source = (
        _SOURCE.replace("@REGISTER_ROWS@", register_rows)
        .replace("@ALIAS_ROWS@", alias_rows)
        .replace("@REGISTER_COUNT@", register_count)
        .replace("@ALIAS_COUNT@", alias_count)
        .replace("@ORACLE_INCLUDE@", oracle_include)
        .replace("@ORACLE_SUPPORT@", oracle_support)
        .replace("@ORACLE_ALLOCATE@", oracle_allocate)
        .replace("@ORACLE_LOAD_REGMAP@", oracle_load_regmap)
        .replace("@ORACLE_RUN_FILES@", oracle_run_files)
    )
    files = {"Regalloc.h": _HEADER, "Regalloc.c": source}
    return files


_HEADER = r"""/* Generated by Archon: deterministic linear-scan allocation. */
#ifndef ARCHON_REGALLOC_H
#define ARCHON_REGALLOC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RA_NO_REGISTER UINT32_MAX
#define RA_NO_SPILL_SLOT UINT32_MAX

enum RA_status {
    RA_OK = 0,
    RA_INVALID_ARGUMENT,
    RA_OUT_OF_MEMORY,
    RA_PARSE_ERROR,
    RA_UNKNOWN_REGISTER,
    RA_CONFLICTING_FIXED_INTERVALS,
    RA_FIXED_ZERO_DEFINITION,
    RA_ORACLE_ERROR,
    RA_IO_ERROR
};

enum RA_stream_kind {
    RA_STREAM_SCHEDULED = 0,
    RA_STREAM_MIR,
    RA_STREAM_REGMAP
};

struct RA_diagnostic {
    enum RA_status status;
    char source[128];
    size_t line;
    char message[256];
};

struct RA_range {
    uint64_t begin;
    uint64_t end;       /* Half-open: [begin, end). */
};

struct RA_interval {
    uint32_t value;
    uint64_t external_value;
    uint32_t width;
    struct RA_range *ranges;
    size_t range_count;
    size_t range_capacity;
    uint32_t fixed_register;
    uint32_t assigned_register;
    uint32_t spill_slot;
    uint32_t spill_cost;
    unsigned has_explicit_ranges;
    unsigned has_canonical_events;
    unsigned fixed;
    unsigned spilled;
};

struct RA_register {
    uint32_t id;
    uint32_t canonical;
    uint64_t external_id;
    uint64_t external_canonical;
    char *name;
    unsigned reserved;
    unsigned hardwired_zero;
};

struct RA_event {
    uint32_t value;
    uint64_t point;
    uint64_t scheduled_instruction;
    unsigned has_scheduled_instruction;
    unsigned definition;
    unsigned needs_schedule;
};

struct RA_alias {
    char *name;
    uint32_t canonical;
};

struct RA_schedule {
    uint32_t instruction;
    uint32_t position;
    uint32_t scheduled_instruction;
    uint64_t external_instruction;
    uint64_t external_scheduled_instruction;
    unsigned external;
};

struct RA_allocator {
    struct RA_interval *intervals;
    size_t interval_count;
    size_t interval_capacity;
    struct RA_register *registers;
    size_t register_count;
    size_t register_capacity;
    struct RA_alias *aliases;
    size_t alias_count;
    size_t alias_capacity;
    struct RA_event *events;
    size_t event_count;
    size_t event_capacity;
    struct RA_schedule *schedule;
    size_t schedule_count;
    size_t schedule_capacity;
    uint32_t spill_count;
    uint64_t scheduled_stream_id;
    uint64_t reg_map_stream_id;
    unsigned allocated;
    unsigned oracle_annotations_applied;
};

void RA_init(struct RA_allocator *allocator);
void RA_destroy(struct RA_allocator *allocator);

enum RA_status RA_add_register(
        struct RA_allocator *allocator, uint32_t id, const char *name);
enum RA_status RA_add_register_alias_group(
        struct RA_allocator *allocator, uint32_t id, uint32_t canonical);
enum RA_status RA_reserve_register(
        struct RA_allocator *allocator, uint32_t id, unsigned hardwired_zero);
enum RA_status RA_add_alias(
        struct RA_allocator *allocator, const char *name, uint32_t canonical);
enum RA_status RA_add_range(
        struct RA_allocator *allocator, uint32_t value,
        uint64_t begin, uint64_t end);
enum RA_status RA_add_use(
        struct RA_allocator *allocator, uint64_t position, uint32_t value);
enum RA_status RA_add_definition(
        struct RA_allocator *allocator, uint64_t position, uint32_t value);
enum RA_status RA_fix_interval(
        struct RA_allocator *allocator, uint32_t value, uint32_t physical);

enum RA_status RA_load_stream(
        struct RA_allocator *allocator, FILE *stream, const char *source,
        enum RA_stream_kind kind, struct RA_diagnostic *diagnostic);
enum RA_status RA_allocate(
        struct RA_allocator *allocator, struct RA_diagnostic *diagnostic);
enum RA_status RA_write(
        FILE *stream, const struct RA_allocator *allocator,
        struct RA_diagnostic *diagnostic);
enum RA_status RA_run_files(
        const char *scheduled_path, const char *mir_path,
        const char *regmap_path, const char *output_path,
        struct RA_diagnostic *diagnostic);

const struct RA_interval *RA_find_interval(
        const struct RA_allocator *allocator, uint32_t value);
const char *RA_register_name(
        const struct RA_allocator *allocator, uint32_t canonical);
const char *RA_status_string(enum RA_status status);

/*
 * Canonical input facts (ARCHON_, RA_, and unprefixed spellings are accepted):
 *
 *   REGMAP_REGISTER(ID, "name")
 *   REGMAP_RESERVED(ID)
 *   REGMAP_ZERO(ID)
 *   REGMAP_ALIAS("name", ID)
 *   REGMAP_ALIAS_GROUP(ID, CANONICAL_ID)
 *   REGMAP_FIXED(VALUE, ID-or-"alias")
 *
 *   SCHEDULED_INSTRUCTION(POSITION, INSTRUCTION_ID)
 *   SCHEDULED_RANGE(VALUE, BEGIN, END)
 *   SCHEDULED_USE(POSITION, VALUE)
 *   SCHEDULED_DEF(POSITION, VALUE)
 *
 *   MIR_USE(INSTRUCTION_ID, VALUE)
 *   MIR_DEF(INSTRUCTION_ID, VALUE)
 *   MIR_RANGE(VALUE, BEGIN, END)
 *   MIR_FIXED(VALUE, ID-or-"alias")
 *
 * MIR uses/defs are mapped through SCHEDULED_INSTRUCTION when a mapping exists.
 * Ranges are half-open and may be repeated to describe lifetime holes.
 *
 * RA_write emits the canonical ARCHON_REGALLOCATED_* Regallocated.i envelope.
 * A compatibility section (enabled by ARCHON_REGALLOC_LEGACY_CALLBACKS) also
 * provides REGALLOC_BEGIN/ASSIGN/SPILL/RELOAD facts.  Spills remain explicit
 * abstract events; no target opcode is fabricated.
 */

#ifdef __cplusplus
}
#endif

#endif
"""


_ORACLE_SUPPORT = r"""
#define RA_ORACLE_MAX_SPILL_COST UINT32_C(1000000)

enum RA_oracle_key_kind {
    RA_ORACLE_KEY_UNKNOWN = 0,
    RA_ORACLE_KEY_FIXED,
    RA_ORACLE_KEY_RESERVED,
    RA_ORACLE_KEY_SPILL_COST,
    RA_ORACLE_KEY_MALFORMED
};

static enum RA_status RA_oracle_error(
        struct RA_diagnostic *diagnostic, const char *source,
        const char *action, ArchonOracleStatus oracle_status,
        const ArchonOracleDiagnostic *oracle_diagnostic) {
    char message[256];
    const char *detail =
        oracle_diagnostic != NULL && oracle_diagnostic->message[0] != '\0'
        ? oracle_diagnostic->message
        : archon_oracle_status_string(oracle_status);
    (void)snprintf(
        message, sizeof message, "%s: %.192s",
        action != NULL ? action : "Oracle failure", detail);
    return RA_diagnose(
        diagnostic, RA_ORACLE_ERROR, source, 0u, message);
}

static enum RA_oracle_key_kind RA_oracle_key(
        const char *key, uint64_t *identifier) {
    static const struct {
        const char *prefix;
        enum RA_oracle_key_kind kind;
    } prefixes[] = {
        {"regalloc.fixed_register.", RA_ORACLE_KEY_FIXED},
        {"regalloc.reserved_register.", RA_ORACLE_KEY_RESERVED},
        {"regalloc.spill_cost.", RA_ORACLE_KEY_SPILL_COST}
    };
    size_t prefix_index;
    for (prefix_index = 0u;
            prefix_index < sizeof prefixes / sizeof prefixes[0];
            ++prefix_index) {
        const char *cursor;
        uint64_t value = UINT64_C(0);
        size_t prefix_size = strlen(prefixes[prefix_index].prefix);
        if (strncmp(key, prefixes[prefix_index].prefix, prefix_size) != 0)
            continue;
        cursor = key + prefix_size;
        if (*cursor == '\0' ||
                (*cursor == '0' && cursor[1] != '\0'))
            return RA_ORACLE_KEY_MALFORMED;
        while (*cursor != '\0') {
            unsigned digit;
            if (*cursor < '0' || *cursor > '9')
                return RA_ORACLE_KEY_MALFORMED;
            digit = (unsigned)(*cursor - '0');
            if (value > (UINT64_MAX - digit) / UINT64_C(10))
                return RA_ORACLE_KEY_MALFORMED;
            value = value * UINT64_C(10) + digit;
            ++cursor;
        }
        *identifier = value;
        return prefixes[prefix_index].kind;
    }
    return RA_ORACLE_KEY_UNKNOWN;
}

static struct RA_interval *RA_oracle_interval(
        struct RA_allocator *allocator, uint64_t external) {
    return RA_external_interval(allocator, external, 0);
}

static enum RA_status RA_oracle_register(
        struct RA_allocator *allocator, uint64_t external,
        uint32_t *physical) {
    struct RA_register *row = RA_external_register(allocator, external);
    if (row != NULL) {
        *physical = row->canonical;
        return RA_OK;
    }
    if (external <= UINT32_MAX) {
        row = RA_mutable_register(allocator, (uint32_t)external);
        if (row != NULL) {
            *physical = row->canonical;
            return RA_OK;
        }
    }
    return RA_UNKNOWN_REGISTER;
}

static enum RA_status RA_validate_oracle_annotations(
        struct RA_allocator *allocator,
        const ArchonOracleAnnotations *annotations,
        const char *sidecar,
        struct RA_diagnostic *diagnostic) {
    size_t index;
    if (annotations->protocol_version != ARCHON_ORACLE_PROTOCOL_VERSION ||
            annotations->stage != ARCHON_ORACLE_STAGE_REGALLOC)
        return RA_diagnose(
            diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
            "Oracle sidecar has the wrong protocol version or stage");
    for (index = 0u; index < annotations->count; ++index) {
        const ArchonOracleAnnotation *entry = &annotations->entries[index];
        uint64_t identifier = UINT64_C(0);
        enum RA_oracle_key_kind kind =
            RA_oracle_key(entry->key, &identifier);
        struct RA_interval *interval;
        uint32_t physical;
        if (kind == RA_ORACLE_KEY_UNKNOWN)
            continue;
        if (kind == RA_ORACLE_KEY_MALFORMED)
            return RA_diagnose(
                diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                "Oracle register-allocation annotation has a malformed key");
        if (kind == RA_ORACLE_KEY_FIXED) {
            if (entry->kind != ARCHON_ORACLE_ANNOTATION_U64)
                return RA_diagnose(
                    diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                    "Oracle fixed-register annotation must be U64");
            interval = RA_oracle_interval(allocator, identifier);
            if (interval == NULL ||
                    RA_oracle_register(
                        allocator, entry->value.u64, &physical) != RA_OK)
                return RA_diagnose(
                    diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                    "Oracle fixed-register annotation names an unknown value or register");
            if (interval->fixed &&
                    interval->fixed_register != physical)
                return RA_diagnose(
                    diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                    "Oracle fixed-register annotation conflicts with validated input");
        } else if (kind == RA_ORACLE_KEY_RESERVED) {
            if (entry->kind != ARCHON_ORACLE_ANNOTATION_BOOL)
                return RA_diagnose(
                    diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                    "Oracle reserved-register annotation must be Boolean");
            if (RA_oracle_register(
                    allocator, identifier, &physical) != RA_OK)
                return RA_diagnose(
                    diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                    "Oracle reserved-register annotation names an unknown register");
        } else {
            if (entry->kind != ARCHON_ORACLE_ANNOTATION_U64 ||
                    entry->value.u64 > RA_ORACLE_MAX_SPILL_COST)
                return RA_diagnose(
                    diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                    "Oracle spill-cost annotation is not a bounded U64");
            if (RA_oracle_interval(allocator, identifier) == NULL)
                return RA_diagnose(
                    diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                    "Oracle spill-cost annotation names an unknown value");
        }
    }
    return RA_OK;
}

static enum RA_status RA_commit_oracle_annotations(
        struct RA_allocator *allocator,
        const ArchonOracleAnnotations *annotations) {
    size_t index;
    for (index = 0u; index < annotations->count; ++index) {
        const ArchonOracleAnnotation *entry = &annotations->entries[index];
        uint64_t identifier = UINT64_C(0);
        enum RA_oracle_key_kind kind =
            RA_oracle_key(entry->key, &identifier);
        struct RA_interval *interval;
        uint32_t physical;
        enum RA_status status;
        if (kind == RA_ORACLE_KEY_UNKNOWN)
            continue;
        if (kind == RA_ORACLE_KEY_FIXED) {
            interval = RA_oracle_interval(allocator, identifier);
            status = RA_oracle_register(
                allocator, entry->value.u64, &physical);
            if (status != RA_OK)
                return status;
            status = RA_fix_interval(
                allocator, interval->value, physical);
            if (status != RA_OK)
                return status;
        } else if (kind == RA_ORACLE_KEY_RESERVED) {
            if (!entry->value.boolean)
                continue;
            status = RA_oracle_register(
                allocator, identifier, &physical);
            if (status != RA_OK)
                return status;
            status = RA_reserve_register(allocator, physical, 0u);
            if (status != RA_OK)
                return status;
        } else if (kind == RA_ORACLE_KEY_SPILL_COST) {
            interval = RA_oracle_interval(allocator, identifier);
            interval->spill_cost = (uint32_t)entry->value.u64;
        }
    }
    return RA_OK;
}

static enum RA_status RA_process_oracle_artifact(
        const char *artifact_path,
        struct RA_diagnostic *diagnostic) {
    ArchonOracleOptions options;
    ArchonOracleDiagnostic oracle_diagnostic;
    ArchonOracleStatus oracle_status;
    if (artifact_path == NULL || artifact_path[0] == '\0')
        return RA_diagnose(
            diagnostic, RA_INVALID_ARGUMENT, "", 0u,
            "Oracle artifact path is empty");
    archon_oracle_options_init(&options);
    options.socket_path = getenv("ARCHON_ORACLE_SOCKET");
    archon_oracle_diagnostic_init(&oracle_diagnostic);
    oracle_status = archon_oracle_process_file(
        artifact_path, ARCHON_ORACLE_STAGE_REGALLOC,
        &options, &oracle_diagnostic);
    if (oracle_status == ARCHON_ORACLE_STATUS_OK ||
            oracle_status == ARCHON_ORACLE_STATUS_INACTIVE)
        return RA_OK;
    return RA_oracle_error(
        diagnostic, artifact_path, "Oracle rejected register map",
        oracle_status, &oracle_diagnostic);
}

static enum RA_status RA_apply_oracle_annotations(
        struct RA_allocator *allocator, const char *artifact_path,
        struct RA_diagnostic *diagnostic) {
    ArchonOracleDiagnostic oracle_diagnostic;
    ArchonOracleAnnotations annotations;
    ArchonOracleStatus oracle_status;
    enum RA_status status;
    char sidecar[ARCHON_ORACLE_MAX_PATH_BYTES + 1u];
    if (allocator->oracle_annotations_applied)
        return RA_OK;
    if (artifact_path == NULL || artifact_path[0] == '\0') {
        allocator->oracle_annotations_applied = 1u;
        return RA_OK;
    }
    oracle_status = archon_oracle_default_annotation_path(
        artifact_path, sidecar, sizeof sidecar);
    if (oracle_status != ARCHON_ORACLE_STATUS_OK)
        return RA_oracle_error(
            diagnostic, artifact_path,
            "cannot derive Oracle annotation path",
            oracle_status, NULL);
    archon_oracle_annotations_init(&annotations);
    archon_oracle_diagnostic_init(&oracle_diagnostic);
    oracle_status = archon_oracle_annotations_load(
        sidecar, &annotations, &oracle_diagnostic);
    if (oracle_status == ARCHON_ORACLE_STATUS_NOT_FOUND) {
        allocator->oracle_annotations_applied = 1u;
        return RA_OK;
    }
    if (oracle_status != ARCHON_ORACLE_STATUS_OK)
        return RA_oracle_error(
            diagnostic, sidecar, "cannot load Oracle annotations",
            oracle_status, &oracle_diagnostic);
    status = RA_validate_oracle_annotations(
        allocator, &annotations, sidecar, diagnostic);
    if (status == RA_OK) {
        status = RA_commit_oracle_annotations(allocator, &annotations);
        if (status != RA_OK)
            status = RA_diagnose(
                diagnostic, RA_ORACLE_ERROR, sidecar, 0u,
                "cannot apply validated Oracle annotations");
    }
    archon_oracle_annotations_dispose(&annotations);
    if (status == RA_OK)
        allocator->oracle_annotations_applied = 1u;
    return status;
}
"""


_SOURCE = r"""/* Generated by Archon: deterministic linear-scan allocation. */
#include "Regalloc.h"
@ORACLE_INCLUDE@

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

struct RA_default_register {
    uint32_t id;
    const char *name;
    unsigned reserved;
    unsigned hardwired_zero;
};

struct RA_default_alias {
    const char *name;
    uint32_t canonical;
};

static const struct RA_default_register RA_default_registers[] = {
@REGISTER_ROWS@
};

static const struct RA_default_alias RA_default_aliases[] = {
@ALIAS_ROWS@
};

static const size_t RA_default_register_count = @REGISTER_COUNT@;
static const size_t RA_default_alias_count = @ALIAS_COUNT@;

static char *RA_duplicate(const char *text) {
    char *copy;
    size_t size;
    if (!text)
        return NULL;
    size = strlen(text) + 1u;
    copy = (char *)malloc(size);
    if (copy)
        memcpy(copy, text, size);
    return copy;
}

static enum RA_status RA_diagnose(
        struct RA_diagnostic *diagnostic, enum RA_status status,
        const char *source, size_t line, const char *message) {
    if (diagnostic) {
        diagnostic->status = status;
        diagnostic->line = line;
        (void)snprintf(diagnostic->source, sizeof diagnostic->source,
            "%s", source ? source : "");
        (void)snprintf(diagnostic->message, sizeof diagnostic->message,
            "%s", message ? message : "");
    }
    return status;
}

static int RA_grow_array(
        void **pointer, size_t *capacity, size_t count, size_t element_size) {
    size_t next;
    void *grown;
    if (count < *capacity)
        return 1;
    if (*capacity > SIZE_MAX / 2u)
        return 0;
    next = *capacity ? *capacity * 2u : 8u;
    if (next <= count)
        next = count + 1u;
    if (element_size && next > SIZE_MAX / element_size)
        return 0;
    grown = realloc(*pointer, next * element_size);
    if (!grown)
        return 0;
    *pointer = grown;
    *capacity = next;
    return 1;
}

static struct RA_register *RA_mutable_register(
        struct RA_allocator *allocator, uint32_t id) {
    size_t i;
    for (i = 0; i < allocator->register_count; ++i)
        if (allocator->registers[i].id == id)
            return &allocator->registers[i];
    return NULL;
}

static const struct RA_register *RA_const_register(
        const struct RA_allocator *allocator, uint32_t id) {
    size_t i;
    for (i = 0; i < allocator->register_count; ++i)
        if (allocator->registers[i].id == id)
            return &allocator->registers[i];
    return NULL;
}

static struct RA_interval *RA_mutable_interval(
        struct RA_allocator *allocator, uint32_t value, int create) {
    struct RA_interval *interval;
    size_t i;
    for (i = 0; i < allocator->interval_count; ++i)
        if (allocator->intervals[i].value == value)
            return &allocator->intervals[i];
    if (!create)
        return NULL;
    if (!RA_grow_array((void **)&allocator->intervals,
            &allocator->interval_capacity, allocator->interval_count,
            sizeof *allocator->intervals))
        return NULL;
    interval = &allocator->intervals[allocator->interval_count++];
    memset(interval, 0, sizeof *interval);
    interval->value = value;
    interval->external_value = value;
    interval->width = 1u;
    interval->fixed_register = RA_NO_REGISTER;
    interval->assigned_register = RA_NO_REGISTER;
    interval->spill_slot = RA_NO_SPILL_SLOT;
    interval->spill_cost = 1u;
    return interval;
}

static struct RA_interval *RA_external_interval(
        struct RA_allocator *allocator, uint64_t external, int create) {
    struct RA_interval *interval;
    uint32_t internal;
    size_t i;
    for (i = 0; i < allocator->interval_count; ++i)
        if (allocator->intervals[i].external_value == external)
            return &allocator->intervals[i];
    if (!create)
        return NULL;
    if (external < UINT32_MAX &&
            !RA_mutable_interval(allocator, (uint32_t)external, 0)) {
        internal = (uint32_t)external;
    } else {
        for (internal = 0u; internal != UINT32_MAX; ++internal)
            if (!RA_mutable_interval(allocator, internal, 0))
                break;
        if (internal == UINT32_MAX)
            return NULL;
    }
    interval = RA_mutable_interval(allocator, internal, 1);
    if (interval)
        interval->external_value = external;
    return interval;
}

static struct RA_register *RA_external_register(
        struct RA_allocator *allocator, uint64_t external) {
    size_t i;
    for (i = 0; i < allocator->register_count; ++i)
        if (allocator->registers[i].external_id == external)
            return &allocator->registers[i];
    return NULL;
}

static enum RA_status RA_resolve_external_register(
        struct RA_allocator *allocator, uint64_t external, uint32_t *internal) {
    struct RA_register *row = RA_external_register(allocator, external);
    if (!row)
        return RA_UNKNOWN_REGISTER;
    *internal = row->canonical;
    return RA_OK;
}

static enum RA_status RA_add_external_register(
        struct RA_allocator *allocator, uint64_t external,
        const char *name, uint32_t *internal) {
    struct RA_register *row;
    uint32_t candidate;
    size_t i;
    row = RA_external_register(allocator, external);
    if (row) {
        *internal = row->canonical;
        return strcmp(row->name, name) == 0 ? RA_OK : RA_INVALID_ARGUMENT;
    }
    /* ArchDB defaults and canonical RegMap rows usually meet by name. */
    for (i = 0; i < allocator->register_count; ++i)
        if (strcmp(allocator->registers[i].name, name) == 0) {
            row = &allocator->registers[i];
            row->external_id = external;
            row->external_canonical = external;
            *internal = row->canonical;
            return RA_OK;
        }
    if (external < UINT32_MAX &&
            !RA_mutable_register(allocator, (uint32_t)external)) {
        candidate = (uint32_t)external;
    } else {
        for (candidate = 0u; candidate != UINT32_MAX; ++candidate)
            if (!RA_mutable_register(allocator, candidate))
                break;
        if (candidate == UINT32_MAX)
            return RA_OUT_OF_MEMORY;
    }
    {
        enum RA_status status = RA_add_register(allocator, candidate, name);
        if (status != RA_OK)
            return status;
    }
    row = RA_mutable_register(allocator, candidate);
    row->external_id = external;
    row->external_canonical = external;
    *internal = candidate;
    return RA_OK;
}

void RA_init(struct RA_allocator *allocator) {
    size_t i;
    if (!allocator)
        return;
    memset(allocator, 0, sizeof *allocator);
    for (i = 0; i < RA_default_register_count; ++i) {
        const struct RA_default_register *row = &RA_default_registers[i];
        if (RA_add_register(allocator, row->id, row->name) != RA_OK)
            break;
        if (row->reserved &&
                RA_reserve_register(allocator, row->id, row->hardwired_zero) != RA_OK)
            break;
    }
    if (i != RA_default_register_count)
        return;
    for (i = 0; i < RA_default_alias_count; ++i)
        if (RA_add_alias(allocator, RA_default_aliases[i].name,
                RA_default_aliases[i].canonical) != RA_OK)
            break;
}

void RA_destroy(struct RA_allocator *allocator) {
    size_t i;
    if (!allocator)
        return;
    for (i = 0; i < allocator->interval_count; ++i)
        free(allocator->intervals[i].ranges);
    for (i = 0; i < allocator->register_count; ++i)
        free(allocator->registers[i].name);
    for (i = 0; i < allocator->alias_count; ++i)
        free(allocator->aliases[i].name);
    free(allocator->intervals);
    free(allocator->registers);
    free(allocator->aliases);
    free(allocator->events);
    free(allocator->schedule);
    memset(allocator, 0, sizeof *allocator);
}

enum RA_status RA_add_register(
        struct RA_allocator *allocator, uint32_t id, const char *name) {
    struct RA_register *row;
    if (!allocator || !name || !*name || id == RA_NO_REGISTER)
        return RA_INVALID_ARGUMENT;
    row = RA_mutable_register(allocator, id);
    if (row) {
        if (strcmp(row->name, name) == 0)
            return RA_OK;
        return RA_add_alias(allocator, name, row->canonical);
    }
    if (!RA_grow_array((void **)&allocator->registers,
            &allocator->register_capacity, allocator->register_count,
            sizeof *allocator->registers))
        return RA_OUT_OF_MEMORY;
    row = &allocator->registers[allocator->register_count++];
    memset(row, 0, sizeof *row);
    row->id = id;
    row->canonical = id;
    row->name = RA_duplicate(name);
    if (!row->name) {
        --allocator->register_count;
        return RA_OUT_OF_MEMORY;
    }
    return RA_OK;
}

enum RA_status RA_add_register_alias_group(
        struct RA_allocator *allocator, uint32_t id, uint32_t canonical) {
    struct RA_register *row, *base;
    enum RA_status status;
    unsigned reserved, hardwired_zero;
    uint32_t old_canonical, new_canonical;
    uint64_t external_canonical;
    size_t i;
    if (!allocator)
        return RA_INVALID_ARGUMENT;
    row = RA_mutable_register(allocator, id);
    base = RA_mutable_register(allocator, canonical);
    if (!row || !base)
        return RA_UNKNOWN_REGISTER;
    old_canonical = row->canonical;
    new_canonical = base->canonical;
    external_canonical = base->external_canonical;
    if (old_canonical == new_canonical)
        return RA_OK;
    reserved = 0u;
    hardwired_zero = 0u;
    for (i = 0; i < allocator->register_count; ++i)
        if (allocator->registers[i].canonical == old_canonical ||
                allocator->registers[i].canonical == new_canonical) {
            reserved = reserved || allocator->registers[i].reserved;
            hardwired_zero =
                hardwired_zero || allocator->registers[i].hardwired_zero;
        }
    for (i = 0; i < allocator->register_count; ++i)
        if (allocator->registers[i].canonical == old_canonical) {
            allocator->registers[i].canonical = new_canonical;
            allocator->registers[i].external_canonical = external_canonical;
        }
    for (i = 0; i < allocator->alias_count; ++i)
        if (allocator->aliases[i].canonical == old_canonical)
            allocator->aliases[i].canonical = new_canonical;
    for (i = 0; i < allocator->interval_count; ++i)
        if (allocator->intervals[i].fixed &&
                allocator->intervals[i].fixed_register == old_canonical)
            allocator->intervals[i].fixed_register = new_canonical;
    status = RA_add_alias(allocator, row->name, new_canonical);
    if (status != RA_OK)
        return status;
    if (reserved || hardwired_zero)
        return RA_reserve_register(
            allocator, new_canonical, hardwired_zero);
    return RA_OK;
}

enum RA_status RA_reserve_register(
        struct RA_allocator *allocator, uint32_t id, unsigned hardwired_zero) {
    struct RA_register *row;
    uint32_t canonical;
    size_t i;
    if (!allocator)
        return RA_INVALID_ARGUMENT;
    row = RA_mutable_register(allocator, id);
    if (!row)
        return RA_UNKNOWN_REGISTER;
    canonical = row->canonical;
    for (i = 0; i < allocator->register_count; ++i) {
        row = &allocator->registers[i];
        if (row->canonical != canonical)
            continue;
        row->reserved = 1u;
        if (hardwired_zero)
            row->hardwired_zero = 1u;
    }
    return RA_OK;
}

enum RA_status RA_add_alias(
        struct RA_allocator *allocator, const char *name, uint32_t canonical) {
    struct RA_alias *row;
    const struct RA_register *physical;
    size_t i;
    if (!allocator || !name || !*name)
        return RA_INVALID_ARGUMENT;
    physical = RA_const_register(allocator, canonical);
    if (!physical)
        return RA_UNKNOWN_REGISTER;
    canonical = physical->canonical;
    for (i = 0; i < allocator->alias_count; ++i) {
        if (strcmp(allocator->aliases[i].name, name) == 0)
            return allocator->aliases[i].canonical == canonical ?
                RA_OK : RA_INVALID_ARGUMENT;
    }
    if (!RA_grow_array((void **)&allocator->aliases,
            &allocator->alias_capacity, allocator->alias_count,
            sizeof *allocator->aliases))
        return RA_OUT_OF_MEMORY;
    row = &allocator->aliases[allocator->alias_count++];
    row->name = RA_duplicate(name);
    if (!row->name) {
        --allocator->alias_count;
        return RA_OUT_OF_MEMORY;
    }
    row->canonical = canonical;
    return RA_OK;
}

enum RA_status RA_add_range(
        struct RA_allocator *allocator, uint32_t value,
        uint64_t begin, uint64_t end) {
    struct RA_interval *interval;
    if (!allocator || begin >= end)
        return RA_INVALID_ARGUMENT;
    interval = RA_mutable_interval(allocator, value, 1);
    if (!interval)
        return RA_OUT_OF_MEMORY;
    if (!RA_grow_array((void **)&interval->ranges,
            &interval->range_capacity, interval->range_count,
            sizeof *interval->ranges))
        return RA_OUT_OF_MEMORY;
    interval->ranges[interval->range_count].begin = begin;
    interval->ranges[interval->range_count].end = end;
    ++interval->range_count;
    interval->has_explicit_ranges = 1u;
    allocator->allocated = 0u;
    return RA_OK;
}

static enum RA_status RA_add_event_with_schedule(
        struct RA_allocator *allocator, uint64_t point, uint32_t value,
        unsigned definition, unsigned needs_schedule,
        uint64_t scheduled_instruction, unsigned has_scheduled_instruction) {
    struct RA_event *event;
    size_t i;
    if (!allocator)
        return RA_INVALID_ARGUMENT;
    if (!RA_mutable_interval(allocator, value, 1))
        return RA_OUT_OF_MEMORY;
    for (i = 0; i < allocator->event_count; ++i) {
        event = &allocator->events[i];
        if (event->point == point && event->value == value &&
                event->definition == definition &&
                event->needs_schedule == needs_schedule) {
            if (has_scheduled_instruction) {
                if (event->has_scheduled_instruction &&
                        event->scheduled_instruction != scheduled_instruction)
                    return RA_INVALID_ARGUMENT;
                event->scheduled_instruction = scheduled_instruction;
                event->has_scheduled_instruction = 1u;
            }
            return RA_OK;
        }
    }
    if (!RA_grow_array((void **)&allocator->events,
            &allocator->event_capacity, allocator->event_count,
            sizeof *allocator->events))
        return RA_OUT_OF_MEMORY;
    event = &allocator->events[allocator->event_count++];
    event->point = point;
    event->value = value;
    event->scheduled_instruction = scheduled_instruction;
    event->has_scheduled_instruction = !!has_scheduled_instruction;
    event->definition = !!definition;
    event->needs_schedule = !!needs_schedule;
    allocator->allocated = 0u;
    return RA_OK;
}

static enum RA_status RA_add_event(
        struct RA_allocator *allocator, uint64_t point, uint32_t value,
        unsigned definition, unsigned needs_schedule) {
    return RA_add_event_with_schedule(
        allocator, point, value, definition, needs_schedule, UINT64_C(0), 0u);
}

enum RA_status RA_add_use(
        struct RA_allocator *allocator, uint64_t position, uint32_t value) {
    return RA_add_event(allocator, position, value, 0u, 0u);
}

enum RA_status RA_add_definition(
        struct RA_allocator *allocator, uint64_t position, uint32_t value) {
    return RA_add_event(allocator, position, value, 1u, 0u);
}

enum RA_status RA_fix_interval(
        struct RA_allocator *allocator, uint32_t value, uint32_t physical) {
    struct RA_interval *interval;
    const struct RA_register *row;
    if (!allocator)
        return RA_INVALID_ARGUMENT;
    row = RA_const_register(allocator, physical);
    if (!row)
        return RA_UNKNOWN_REGISTER;
    interval = RA_mutable_interval(allocator, value, 1);
    if (!interval)
        return RA_OUT_OF_MEMORY;
    if (interval->fixed && interval->fixed_register != row->canonical)
        return RA_INVALID_ARGUMENT;
    interval->fixed = 1u;
    interval->fixed_register = row->canonical;
    allocator->allocated = 0u;
    return RA_OK;
}

static enum RA_status RA_add_schedule(
        struct RA_allocator *allocator, uint32_t instruction, uint32_t position,
        uint32_t scheduled_instruction) {
    struct RA_schedule *row;
    size_t i;
    for (i = 0; i < allocator->schedule_count; ++i) {
        row = &allocator->schedule[i];
        if (row->instruction == instruction)
            return row->position == position &&
                row->scheduled_instruction == scheduled_instruction ?
                RA_OK : RA_PARSE_ERROR;
    }
    if (!RA_grow_array((void **)&allocator->schedule,
            &allocator->schedule_capacity, allocator->schedule_count,
            sizeof *allocator->schedule))
        return RA_OUT_OF_MEMORY;
    row = &allocator->schedule[allocator->schedule_count++];
    row->instruction = instruction;
    row->position = position;
    row->scheduled_instruction = scheduled_instruction;
    row->external_instruction = instruction;
    row->external_scheduled_instruction = scheduled_instruction;
    row->external = 0u;
    return RA_OK;
}

static enum RA_status RA_external_instruction_key(
        struct RA_allocator *allocator, uint64_t external,
        int create, uint32_t *key) {
    struct RA_schedule *row;
    uint32_t candidate;
    size_t i;
    for (i = 0; i < allocator->schedule_count; ++i) {
        row = &allocator->schedule[i];
        if (row->external && row->external_instruction == external) {
            *key = row->instruction;
            return RA_OK;
        }
    }
    if (!create)
        return RA_PARSE_ERROR;
    for (candidate = 0u; candidate != UINT32_MAX; ++candidate) {
        int used = 0;
        for (i = 0; i < allocator->schedule_count; ++i)
            if (allocator->schedule[i].instruction == candidate) {
                used = 1;
                break;
            }
        if (!used)
            break;
    }
    if (candidate == UINT32_MAX ||
            !RA_grow_array((void **)&allocator->schedule,
                &allocator->schedule_capacity, allocator->schedule_count,
                sizeof *allocator->schedule))
        return RA_OUT_OF_MEMORY;
    row = &allocator->schedule[allocator->schedule_count++];
    row->instruction = candidate;
    row->position = UINT32_MAX;
    row->scheduled_instruction = candidate;
    row->external_instruction = external;
    row->external_scheduled_instruction = external;
    row->external = 1u;
    *key = candidate;
    return RA_OK;
}

static enum RA_status RA_add_external_schedule(
        struct RA_allocator *allocator, uint64_t external_key,
        uint32_t position, uint64_t external_scheduled) {
    uint32_t key;
    enum RA_status status = RA_external_instruction_key(
        allocator, external_key, 1, &key);
    size_t i;
    if (status != RA_OK)
        return status;
    for (i = 0; i < allocator->schedule_count; ++i) {
        struct RA_schedule *row = &allocator->schedule[i];
        if (row->external && row->instruction == key) {
            if (row->position != UINT32_MAX &&
                    (row->position != position ||
                     row->external_scheduled_instruction != external_scheduled))
                return RA_PARSE_ERROR;
            row->position = position;
            row->external_scheduled_instruction = external_scheduled;
            return RA_OK;
        }
    }
    return RA_PARSE_ERROR;
}

static char *RA_trim(char *text) {
    char *end;
    while (*text && isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end != text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

static void RA_upper(char *text) {
    while (*text) {
        *text = (char)toupper((unsigned char)*text);
        ++text;
    }
}

static int RA_parse_u32(const char *text, uint32_t *value) {
    char buffer[256], *begin, *end;
    unsigned long parsed;
    size_t size;
    if (!text || !value)
        return 0;
    while (isspace((unsigned char)*text))
        ++text;
    size = strlen(text);
    if (size >= sizeof buffer)
        return 0;
    memcpy(buffer, text, size + 1u);
    begin = RA_trim(buffer);
    if (*begin == '"' && size >= 2u) {
        ++begin;
        end = strrchr(begin, '"');
        if (!end)
            return 0;
        *end = '\0';
    }
    for (size = 0; size < 4u; ++size) {
        static const char *const wrappers[] = {
            "UINT32_C(", "UINT64_C(", "INT32_C(", "INT64_C("
        };
        size_t prefix = strlen(wrappers[size]);
        size_t length = strlen(begin);
        if (length > prefix && strncmp(begin, wrappers[size], prefix) == 0 &&
                begin[length - 1u] == ')') {
            begin[length - 1u] = '\0';
            begin += prefix;
            break;
        }
    }
    while (*begin == '%' || *begin == '$')
        ++begin;
    if ((*begin == 't' || *begin == 'T' || *begin == 'v' || *begin == 'V' ||
            *begin == 'r' || *begin == 'R' || *begin == 'L') &&
            isdigit((unsigned char)begin[1]))
        ++begin;
    if (*begin == '-' || *begin == '+')
        return 0;
    errno = 0;
    parsed = strtoul(begin, &end, 0);
    if (errno || end == begin || parsed > UINT32_MAX)
        return 0;
    while (isspace((unsigned char)*end))
        ++end;
    if (*end == 'u' || *end == 'U')
        ++end;
    while (isspace((unsigned char)*end))
        ++end;
    if (*end)
        return 0;
    *value = (uint32_t)parsed;
    return 1;
}

static int RA_parse_u64(const char *text, uint64_t *value) {
    char buffer[256], *begin, *end;
    unsigned long long parsed;
    size_t size, i;
    static const char *const wrappers[] = {
        "UINT64_C(", "UINT32_C(", "INT64_C(", "INT32_C("
    };
    if (!text || !value)
        return 0;
    while (isspace((unsigned char)*text))
        ++text;
    size = strlen(text);
    if (size >= sizeof buffer)
        return 0;
    memcpy(buffer, text, size + 1u);
    begin = RA_trim(buffer);
    for (i = 0; i < sizeof wrappers / sizeof wrappers[0]; ++i) {
        size_t prefix = strlen(wrappers[i]);
        size_t length = strlen(begin);
        if (length > prefix && strncmp(begin, wrappers[i], prefix) == 0 &&
                begin[length - 1u] == ')') {
            begin[length - 1u] = '\0';
            begin += prefix;
            break;
        }
    }
    if (*begin == '-' || *begin == '+')
        return 0;
    errno = 0;
    parsed = strtoull(begin, &end, 0);
    if (errno || end == begin)
        return 0;
    while (isspace((unsigned char)*end))
        ++end;
    if (*end == 'u' || *end == 'U')
        ++end;
    if (*end == 'l' || *end == 'L')
        ++end;
    if (*end == 'l' || *end == 'L')
        ++end;
    while (isspace((unsigned char)*end))
        ++end;
    if (*end)
        return 0;
    *value = (uint64_t)parsed;
    return 1;
}

static int RA_unquote(const char *text, char *output, size_t capacity) {
    const char *begin, *end, *cursor;
    size_t length, written = 0;
    if (!text || !output || !capacity)
        return 0;
    begin = text;
    while (isspace((unsigned char)*begin))
        ++begin;
    end = begin + strlen(begin);
    while (end != begin && isspace((unsigned char)end[-1]))
        --end;
    if (end - begin < 2 || *begin != '"' || end[-1] != '"') {
        length = (size_t)(end - begin);
        if (length + 1u > capacity)
            return 0;
        memcpy(output, begin, length);
        output[length] = '\0';
        return length != 0u;
    }
    ++begin;
    --end;
    for (cursor = begin; cursor < end; ++cursor) {
        unsigned char value = (unsigned char)*cursor;
        if (value == '\\') {
            unsigned digits = 0, octal = 0;
            if (++cursor == end)
                return 0;
            value = (unsigned char)*cursor;
            switch (value) {
            case 'a': value = '\a'; break;
            case 'b': value = '\b'; break;
            case 't': value = '\t'; break;
            case 'n': value = '\n'; break;
            case 'v': value = '\v'; break;
            case 'f': value = '\f'; break;
            case 'r': value = '\r'; break;
            case '"': case '\\': case '?': case '\'': break;
            default:
                if (value < '0' || value > '7')
                    return 0;
                do {
                    octal = octal * 8u + (unsigned)(*cursor - '0');
                    ++digits;
                    ++cursor;
                } while (cursor < end && digits < 3u &&
                        *cursor >= '0' && *cursor <= '7');
                --cursor;
                if (octal > 255u)
                    return 0;
                value = (unsigned char)octal;
                break;
            }
        } else if (value == '"') {
            return 0;
        }
        if (written + 1u >= capacity || value == '\0')
            return 0;
        output[written++] = (char)value;
    }
    output[written] = '\0';
    return written != 0u;
}

static int RA_invocation(
        char *line, char *name, size_t name_capacity,
        char arguments[][256], size_t *argument_count) {
    char *cursor, *open, *close, *start;
    size_t count = 0, length;
    int quote = 0, escape = 0, depth = 0;
    cursor = RA_trim(line);
    if (!*cursor || *cursor == '#' ||
            (cursor[0] == '/' && (cursor[1] == '/' || cursor[1] == '*')))
        return 0;
    open = strchr(cursor, '(');
    close = strrchr(cursor, ')');
    if (!open || !close || close < open)
        return -1;
    length = (size_t)(open - cursor);
    while (length && isspace((unsigned char)cursor[length - 1u]))
        --length;
    if (!length || length + 1u > name_capacity)
        return -1;
    memcpy(name, cursor, length);
    name[length] = '\0';
    RA_upper(name);
    start = open + 1;
    for (cursor = start; cursor <= close; ++cursor) {
        int split = cursor == close;
        if (!split) {
            if (escape) {
                escape = 0;
            } else if (*cursor == '\\' && quote) {
                escape = 1;
            } else if (*cursor == '"') {
                quote = !quote;
            } else if (!quote && *cursor == '(') {
                ++depth;
            } else if (!quote && *cursor == ')' && depth) {
                --depth;
            } else if (!quote && !depth && *cursor == ',') {
                split = 1;
            }
        }
        if (split) {
            char *trimmed;
            if (cursor == start && count == 0u) {
                start = cursor + 1;
                continue;
            }
            if (count >= 16u || (size_t)(cursor - start) >= sizeof arguments[count])
                return -1;
            memcpy(arguments[count], start, (size_t)(cursor - start));
            arguments[count][cursor - start] = '\0';
            trimmed = RA_trim(arguments[count]);
            if (trimmed != arguments[count])
                memmove(arguments[count], trimmed, strlen(trimmed) + 1u);
            ++count;
            start = cursor + 1;
        }
    }
    if (quote || depth)
        return -1;
    *argument_count = count;
    return 1;
}

static int RA_name_has(const char *name, const char *part) {
    return strstr(name, part) != NULL;
}

static int RA_name_ends(const char *name, const char *suffix) {
    size_t name_size = strlen(name), suffix_size = strlen(suffix);
    return name_size >= suffix_size &&
        strcmp(name + name_size - suffix_size, suffix) == 0;
}

static enum RA_status RA_resolve_register_token(
        const struct RA_allocator *allocator, const char *token, uint32_t *id) {
    char name[256];
    size_t i;
    if (RA_parse_u32(token, id)) {
        const struct RA_register *row = RA_const_register(allocator, *id);
        if (!row)
            return RA_UNKNOWN_REGISTER;
        *id = row->canonical;
        return RA_OK;
    }
    if (!RA_unquote(token, name, sizeof name))
        return RA_PARSE_ERROR;
    for (i = 0; i < allocator->alias_count; ++i)
        if (strcmp(allocator->aliases[i].name, name) == 0) {
            *id = allocator->aliases[i].canonical;
            return RA_OK;
        }
    for (i = 0; i < allocator->register_count; ++i)
        if (strcmp(allocator->registers[i].name, name) == 0) {
            *id = allocator->registers[i].canonical;
            return RA_OK;
        }
    return RA_UNKNOWN_REGISTER;
}

@ORACLE_SUPPORT@

static enum RA_status RA_parse_regmap(
        struct RA_allocator *allocator, const char *name,
        char arguments[][256], size_t count) {
    uint32_t first, second, begin, end;
    uint64_t stream_id, external_first, external_second, scheduled_stream_id;
    uint64_t range_begin, range_end;
    char text[256];
    enum RA_status status;
    if (RA_name_ends(name, "ARCHON_REG_MAP_BEGIN")) {
        /*
         * Schema 1 has target/scheduled/MIR metadata and seven body counts
         * (13 arguments total).  Accept the earlier seven-argument draft too,
         * but only the current form carries a scheduled-stream identity.
         */
        if ((count != 13u && count != 7u) ||
                !RA_parse_u64(arguments[2], &stream_id) || !stream_id)
            return RA_PARSE_ERROR;
        if (count == 13u) {
            if (!RA_parse_u64(arguments[4], &scheduled_stream_id))
                return RA_PARSE_ERROR;
            if (scheduled_stream_id) {
                if (allocator->scheduled_stream_id &&
                        allocator->scheduled_stream_id != scheduled_stream_id)
                    return RA_PARSE_ERROR;
                allocator->scheduled_stream_id = scheduled_stream_id;
            }
        }
        allocator->reg_map_stream_id = stream_id;
        return RA_OK;
    }
    if (RA_name_ends(name, "ARCHON_REG_MAP_REGISTER")) {
        if (count != 6u || !RA_parse_u64(arguments[0], &external_first) ||
                !external_first ||
                !RA_unquote(arguments[2], text, sizeof text))
            return RA_PARSE_ERROR;
        return RA_add_external_register(
            allocator, external_first, text, &first);
    }
    if (RA_name_ends(name, "ARCHON_REG_MAP_ALIAS")) {
        if (count != 7u || !RA_unquote(arguments[2], text, sizeof text) ||
                !RA_parse_u32(arguments[4], &begin) ||
                !RA_parse_u32(arguments[5], &end) || !end)
            return RA_PARSE_ERROR;
        if (!RA_parse_u64(arguments[3], &external_first) || !external_first)
            return RA_PARSE_ERROR;
        status = RA_resolve_external_register(
            allocator, external_first, &first);
        if (status != RA_OK)
            return status;
        status = RA_add_alias(allocator, text, first);
        if (status != RA_OK)
            return status;
        if (strstr(arguments[6], "DISCARD") != NULL)
            return RA_reserve_register(allocator, first, 1u);
        return RA_OK;
    }
    if (RA_name_ends(name, "ARCHON_REG_MAP_RESERVED")) {
        if (count != 5u || !RA_parse_u64(arguments[2], &external_first) ||
                !external_first)
            return RA_PARSE_ERROR;
        status = RA_resolve_external_register(
            allocator, external_first, &first);
        return status == RA_OK ?
            RA_reserve_register(allocator, first, 0u) : status;
    }
    if (RA_name_ends(name, "ARCHON_REG_MAP_VREG")) {
        struct RA_interval *interval;
        if (count != 5u || !RA_parse_u64(arguments[0], &external_first) ||
                !external_first || !RA_parse_u32(arguments[3], &first) ||
                !first)
            return RA_PARSE_ERROR;
        interval = RA_external_interval(allocator, external_first, 1);
        if (!interval)
            return RA_OUT_OF_MEMORY;
        interval->width = first;
        return RA_OK;
    }
    if (RA_name_ends(name, "ARCHON_REG_MAP_INTERVAL")) {
        struct RA_interval *interval;
        if (count != 6u ||
                !RA_parse_u64(arguments[2], &external_first) ||
                !external_first ||
                !RA_parse_u64(arguments[3], &range_begin) ||
                !RA_parse_u64(arguments[4], &range_end) ||
                range_begin >= range_end)
            return RA_PARSE_ERROR;
        interval = RA_external_interval(allocator, external_first, 1);
        if (!interval)
            return RA_OUT_OF_MEMORY;
        return RA_add_range(
            allocator, interval->value, range_begin, range_end);
    }
    if (RA_name_ends(name, "ARCHON_REG_MAP_EVENT")) {
        struct RA_interval *interval;
        unsigned definition;
        if (count != 7u ||
                !RA_parse_u64(arguments[2], &external_first) ||
                !external_first ||
                !RA_parse_u64(arguments[3], &range_begin) ||
                !RA_parse_u64(arguments[5], &external_second) ||
                !external_second)
            return RA_PARSE_ERROR;
        if (strstr(arguments[4], "EARLY_CLOBBER") != NULL ||
                strstr(arguments[4], "_DEF") != NULL) {
            definition = 1u;
        } else if (strstr(arguments[4], "_USE") != NULL) {
            definition = 0u;
        } else {
            return RA_PARSE_ERROR;
        }
        interval = RA_external_interval(allocator, external_second, 1);
        if (!interval)
            return RA_OUT_OF_MEMORY;
        status = RA_add_event_with_schedule(
            allocator, range_begin, interval->value, definition, 0u,
            external_first, 1u);
        if (status == RA_OK)
            interval->has_canonical_events = 1u;
        return status;
    }
    if (RA_name_ends(name, "ARCHON_REG_MAP_FIXED")) {
        struct RA_interval *interval;
        if (count != 5u ||
                !RA_parse_u64(arguments[2], &external_first) ||
                !external_first ||
                !RA_parse_u64(arguments[3], &external_second) ||
                !external_second)
            return RA_PARSE_ERROR;
        interval = RA_external_interval(allocator, external_first, 1);
        if (!interval)
            return RA_OUT_OF_MEMORY;
        status = RA_resolve_external_register(
            allocator, external_second, &second);
        return status == RA_OK ?
            RA_fix_interval(allocator, interval->value, second) : status;
    }
    /*
     * ISched's map uses inclusive cycle endpoints and carries an unconstrained
     * virtual-register hint.  The latter is deliberately not mistaken for a
     * physical register; target capacity is enforced below by this allocator.
     */
    if (RA_name_ends(name, "ISCHED_REG")) {
        if (count != 5u || !RA_parse_u32(arguments[0], &first) ||
                !RA_parse_u32(arguments[3], &begin) ||
                !RA_parse_u32(arguments[4], &end) || begin > end ||
                end == UINT32_MAX)
            return RA_PARSE_ERROR;
        return RA_add_range(allocator, first, begin, end + 1u);
    }
    if (RA_name_has(name, "ALIAS_GROUP") || RA_name_has(name, "ALIASING_REGISTER")) {
        if (count != 2u || !RA_parse_u32(arguments[0], &first) ||
                !RA_parse_u32(arguments[1], &second))
            return RA_PARSE_ERROR;
        return RA_add_register_alias_group(allocator, first, second);
    }
    if (RA_name_has(name, "ALIAS")) {
        if (count != 2u || !RA_unquote(arguments[0], text, sizeof text))
            return RA_PARSE_ERROR;
        status = RA_resolve_register_token(allocator, arguments[1], &first);
        return status == RA_OK ? RA_add_alias(allocator, text, first) : status;
    }
    if (RA_name_has(name, "FIXED")) {
        if (count < 2u || !RA_parse_u32(arguments[0], &first))
            return RA_PARSE_ERROR;
        status = RA_resolve_register_token(allocator, arguments[1], &second);
        return status == RA_OK ? RA_fix_interval(allocator, first, second) : status;
    }
    if (RA_name_has(name, "ZERO")) {
        if (count != 1u)
            return RA_PARSE_ERROR;
        status = RA_resolve_register_token(allocator, arguments[0], &first);
        return status == RA_OK ? RA_reserve_register(allocator, first, 1u) : status;
    }
    if (RA_name_has(name, "RESERV")) {
        if (count != 1u)
            return RA_PARSE_ERROR;
        status = RA_resolve_register_token(allocator, arguments[0], &first);
        return status == RA_OK ? RA_reserve_register(allocator, first, 0u) : status;
    }
    if (RA_name_has(name, "REGISTER") || RA_name_has(name, "PHYSREG")) {
        if (count < 2u || !RA_parse_u32(arguments[0], &first) ||
                !RA_unquote(arguments[1], text, sizeof text))
            return RA_PARSE_ERROR;
        return RA_add_register(allocator, first, text);
    }
    return RA_OK; /* Forward-compatible map fact. */
}

static enum RA_status RA_parse_live_fact(
        struct RA_allocator *allocator, const char *name,
        char arguments[][256], size_t count, enum RA_stream_kind kind) {
    uint32_t a, b, c;
    uint64_t stream_id, external_a, external_b, range_begin, range_end;
    enum RA_status status;
    unsigned mapped = kind == RA_STREAM_MIR;
    if (kind == RA_STREAM_SCHEDULED &&
            RA_name_ends(name, "ARCHON_SCHEDULED_BEGIN")) {
        if ((count != 7u && count != 6u) ||
                !RA_parse_u64(arguments[2], &stream_id) || !stream_id)
            return RA_PARSE_ERROR;
        if (allocator->scheduled_stream_id &&
                allocator->scheduled_stream_id != stream_id)
            return RA_PARSE_ERROR;
        allocator->scheduled_stream_id = stream_id;
        return RA_OK;
    }
    if (kind == RA_STREAM_SCHEDULED &&
            RA_name_ends(name, "ARCHON_SCHEDULED_INSTRUCTION")) {
        if ((count != 8u && count != 7u) ||
                !RA_parse_u64(arguments[0], &external_a) || !external_a ||
                !RA_parse_u32(arguments[1], &b) ||
                !RA_parse_u64(arguments[2], &external_b) || !external_b)
            return RA_PARSE_ERROR;
        status = RA_add_external_schedule(
            allocator, external_a, b, external_a);
        if (status != RA_OK)
            return status;
        return RA_add_external_schedule(
            allocator, external_b, b, external_a);
    }
    if (kind == RA_STREAM_SCHEDULED &&
            RA_name_ends(name, "ARCHON_SCHEDULED_REGION"))
        return count == 6u ? RA_OK : RA_PARSE_ERROR;
    if (kind == RA_STREAM_MIR && RA_name_ends(name, "ARCHON_MIR_VREG")) {
        struct RA_interval *interval;
        if (count != 5u || !RA_parse_u64(arguments[0], &external_a) ||
                !external_a ||
                !RA_parse_u32(arguments[3], &b) || b == 0u)
            return RA_PARSE_ERROR;
        interval = RA_external_interval(allocator, external_a, 1);
        if (!interval)
            return RA_OUT_OF_MEMORY;
        interval->width = b;
        return RA_OK;
    }
    if (kind == RA_STREAM_MIR &&
            RA_name_ends(name, "ARCHON_MIR_INSTRUCTION")) {
        size_t schedule_index;
        if (count != 10u || !RA_parse_u64(arguments[0], &external_a) ||
                !external_a ||
                !RA_parse_u32(arguments[2], &b))
            return RA_PARSE_ERROR;
        status = RA_external_instruction_key(
            allocator, external_a, 1, &a);
        if (status != RA_OK)
            return status;
        for (schedule_index = 0;
                schedule_index < allocator->schedule_count; ++schedule_index)
            if (allocator->schedule[schedule_index].instruction == a) {
                if (allocator->schedule[schedule_index].position == UINT32_MAX)
                    allocator->schedule[schedule_index].position = b;
                return RA_OK;
            }
        return RA_PARSE_ERROR;
    }
    if (kind == RA_STREAM_MIR &&
            (RA_name_ends(name, "ARCHON_MIR_DEF") ||
             RA_name_ends(name, "ARCHON_MIR_USE"))) {
        struct RA_interval *interval;
        unsigned definition = RA_name_ends(name, "ARCHON_MIR_DEF");
        if (count != 4u ||
                !RA_parse_u64(arguments[0], &external_a) ||
                !external_a ||
                !RA_parse_u64(arguments[2], &external_b) ||
                !external_b)
            return RA_PARSE_ERROR;
        status = RA_external_instruction_key(
            allocator, external_a, 1, &a);
        if (status != RA_OK)
            return status;
        interval = RA_external_interval(allocator, external_b, 1);
        if (!interval)
            return RA_OUT_OF_MEMORY;
        if (interval->has_canonical_events)
            return RA_OK;
        return RA_add_event(
            allocator, a, interval->value, definition, 1u);
    }
    if (kind == RA_STREAM_SCHEDULED &&
            RA_name_ends(name, "ISCHED_SCHEDULE_OP")) {
        if (count < 2u || !RA_parse_u32(arguments[0], &a))
            return RA_PARSE_ERROR;
        if (allocator->schedule_count > UINT32_MAX)
            return RA_OUT_OF_MEMORY;
        return RA_add_schedule(
            allocator, a, (uint32_t)allocator->schedule_count, a);
    }
    if (kind == RA_STREAM_MIR &&
            RA_name_ends(name, "ISCHED_MIR_DEP")) {
        uint32_t value;
        int is_data;
        if (count < 7u)
            return RA_PARSE_ERROR;
        is_data = strstr(arguments[4], "DATA") != NULL;
        if (!is_data && RA_parse_u32(arguments[4], &c))
            is_data = (c & 1u) != 0u;
        if (!is_data)
            return RA_OK;
        if (!RA_parse_u32(arguments[0], &a) ||
                !RA_parse_u32(arguments[1], &b) ||
                !RA_parse_u32(arguments[6], &value))
            return RA_PARSE_ERROR;
        status = RA_add_event(allocator, a, value, 1u, 1u);
        return status == RA_OK ?
            RA_add_event(allocator, b, value, 0u, 1u) : status;
    }
    if (kind == RA_STREAM_MIR && RA_name_ends(name, "MIR_INPUT_VALUE")) {
        if (count < 3u || !RA_parse_u32(arguments[0], &a) ||
                !RA_parse_u32(arguments[2], &b))
            return RA_PARSE_ERROR;
        return RA_add_event(allocator, a, b, 0u, 1u);
    }
    if (kind == RA_STREAM_MIR && RA_name_ends(name, "MIR_OPERAND")) {
        if (count < 2u || !RA_parse_u32(arguments[0], &a) ||
                !RA_parse_u32(arguments[1], &b))
            return RA_PARSE_ERROR;
        return RA_add_event(allocator, a, b, 0u, 1u);
    }
    if (kind == RA_STREAM_MIR && RA_name_ends(name, "MIR_OP") &&
            !RA_name_ends(name, "ISCHED_MIR_OP")) {
        if (count < 5u || !RA_parse_u32(arguments[0], &a))
            return RA_PARSE_ERROR;
        if (!RA_parse_u32(arguments[4], &b))
            return strstr(arguments[4], "NO_VALUE") != NULL ?
                RA_OK : RA_PARSE_ERROR;
        return RA_add_event(allocator, a, b, 1u, 1u);
    }
    if (kind == RA_STREAM_MIR &&
            (RA_name_ends(name, "MIR_NODE") ||
             RA_name_ends(name, "MIR_CONSTANT"))) {
        if (!count || !RA_parse_u32(arguments[0], &a))
            return RA_PARSE_ERROR;
        return RA_add_event(allocator, a, a, 1u, 1u);
    }
    if (RA_name_has(name, "RANGE") || RA_name_has(name, "INTERVAL")) {
        if (count < 3u || !RA_parse_u32(arguments[0], &a) ||
                !RA_parse_u64(arguments[1], &range_begin) ||
                !RA_parse_u64(arguments[2], &range_end))
            return RA_PARSE_ERROR;
        status = RA_add_range(allocator, a, range_begin, range_end);
        if (status != RA_OK)
            return status;
        if (RA_name_has(name, "FIXED") && count >= 4u) {
            status = RA_resolve_register_token(allocator, arguments[3], &b);
            return status == RA_OK ? RA_fix_interval(allocator, a, b) : status;
        }
        return RA_OK;
    }
    if (RA_name_has(name, "FIXED")) {
        if (count < 2u || !RA_parse_u32(arguments[0], &a))
            return RA_PARSE_ERROR;
        status = RA_resolve_register_token(allocator, arguments[1], &b);
        return status == RA_OK ? RA_fix_interval(allocator, a, b) : status;
    }
    if (kind == RA_STREAM_SCHEDULED &&
            (RA_name_has(name, "INSTRUCTION") || RA_name_has(name, "ORDER") ||
             RA_name_has(name, "SCHEDULE")) &&
            !RA_name_has(name, "BEGIN") && !RA_name_has(name, "END")) {
        if (count < 2u || !RA_parse_u32(arguments[0], &a) ||
                !RA_parse_u32(arguments[1], &b))
            return RA_PARSE_ERROR;
        return RA_add_schedule(allocator, b, a, b);
    }
    if (RA_name_has(name, "USE")) {
        if (count < 2u || !RA_parse_u32(arguments[0], &a) ||
                !RA_parse_u32(arguments[1], &b))
            return RA_PARSE_ERROR;
        return RA_add_event(allocator, a, b, 0u, mapped);
    }
    if (RA_name_has(name, "DEF")) {
        if (count < 2u || !RA_parse_u32(arguments[0], &a) ||
                !RA_parse_u32(arguments[1], &b))
            return RA_PARSE_ERROR;
        return RA_add_event(allocator, a, b, 1u, mapped);
    }

    /*
     * Derive uses and definitions from the canonical TAC-like MIR spellings.
     * The first argument is an instruction id and the second is a destination
     * for value-producing operations.  This is enough for a SECD-style
     * environment where stack/environment values have already become temps.
     */
    if (kind != RA_STREAM_MIR || count < 2u ||
            !RA_parse_u32(arguments[0], &a))
        return RA_OK;
    if (RA_name_has(name, "CONSTANT") || RA_name_has(name, "PARAMETER")) {
        if (!RA_parse_u32(arguments[1], &b))
            return RA_PARSE_ERROR;
        return RA_add_event(allocator, a, b, 1u, 1u);
    }
    if (RA_name_has(name, "COPY") || RA_name_has(name, "UNARY") ||
            RA_name_has(name, "LOAD")) {
        if (count < 3u || !RA_parse_u32(arguments[1], &b) ||
                !RA_parse_u32(arguments[count - 1u], &c))
            return RA_PARSE_ERROR;
        status = RA_add_event(allocator, a, b, 1u, 1u);
        return status == RA_OK ? RA_add_event(allocator, a, c, 0u, 1u) : status;
    }
    if (RA_name_has(name, "BINARY")) {
        uint32_t d;
        if (count < 4u || !RA_parse_u32(arguments[1], &b) ||
                !RA_parse_u32(arguments[count - 2u], &c) ||
                !RA_parse_u32(arguments[count - 1u], &d))
            return RA_PARSE_ERROR;
        status = RA_add_event(allocator, a, b, 1u, 1u);
        if (status == RA_OK)
            status = RA_add_event(allocator, a, c, 0u, 1u);
        return status == RA_OK ? RA_add_event(allocator, a, d, 0u, 1u) : status;
    }
    if (RA_name_has(name, "STORE")) {
        if (count < 3u || !RA_parse_u32(arguments[count - 2u], &b) ||
                !RA_parse_u32(arguments[count - 1u], &c))
            return RA_PARSE_ERROR;
        status = RA_add_event(allocator, a, b, 0u, 1u);
        return status == RA_OK ? RA_add_event(allocator, a, c, 0u, 1u) : status;
    }
    if (RA_name_has(name, "BRANCH") || RA_name_has(name, "RETURN") ||
            RA_name_has(name, "EFFECT")) {
        if (!RA_parse_u32(arguments[1], &b))
            return RA_OK; /* A void return/effect has no value use. */
        return RA_add_event(allocator, a, b, 0u, 1u);
    }
    return RA_OK;
}

enum RA_status RA_load_stream(
        struct RA_allocator *allocator, FILE *stream, const char *source,
        enum RA_stream_kind kind, struct RA_diagnostic *diagnostic) {
    char line[4096], name[128], arguments[16][256];
    enum RA_status status;
    size_t line_number = 0, count;
    int parsed;
    if (!allocator || !stream || kind < RA_STREAM_SCHEDULED || kind > RA_STREAM_REGMAP)
        return RA_diagnose(diagnostic, RA_INVALID_ARGUMENT, source, 0, "invalid stream");
    while (fgets(line, sizeof line, stream)) {
        ++line_number;
        if (!strchr(line, '\n') && !feof(stream))
            return RA_diagnose(diagnostic, RA_PARSE_ERROR, source, line_number,
                "macro line is too long");
        parsed = RA_invocation(line, name, sizeof name, arguments, &count);
        if (parsed == 0)
            continue;
        if (parsed < 0)
            return RA_diagnose(diagnostic, RA_PARSE_ERROR, source, line_number,
                "malformed macro invocation");
        status = kind == RA_STREAM_REGMAP ?
            RA_parse_regmap(allocator, name, arguments, count) :
            RA_parse_live_fact(allocator, name, arguments, count, kind);
        if (status != RA_OK)
            return RA_diagnose(diagnostic, status, source, line_number,
                status == RA_UNKNOWN_REGISTER ? "macro names an unknown physical register" :
                "invalid register-allocation fact");
    }
    if (ferror(stream))
        return RA_diagnose(diagnostic, RA_IO_ERROR, source, line_number,
            "cannot read macro stream");
    return RA_diagnose(diagnostic, RA_OK, source, line_number, "ok");
}

static int RA_range_compare(const void *left, const void *right) {
    const struct RA_range *a = (const struct RA_range *)left;
    const struct RA_range *b = (const struct RA_range *)right;
    if (a->begin != b->begin)
        return a->begin < b->begin ? -1 : 1;
    if (a->end != b->end)
        return a->end < b->end ? -1 : 1;
    return 0;
}

static void RA_normalize_ranges(struct RA_interval *interval) {
    size_t read, write;
    if (interval->range_count < 2u)
        return;
    qsort(interval->ranges, interval->range_count,
        sizeof *interval->ranges, RA_range_compare);
    write = 0;
    for (read = 1; read < interval->range_count; ++read) {
        if (interval->ranges[read].begin <= interval->ranges[write].end) {
            if (interval->ranges[read].end > interval->ranges[write].end)
                interval->ranges[write].end = interval->ranges[read].end;
        } else {
            ++write;
            interval->ranges[write] = interval->ranges[read];
        }
    }
    interval->range_count = write + 1u;
}

static int RA_schedule_position(
        const struct RA_allocator *allocator, uint64_t instruction,
        uint32_t *position) {
    size_t i;
    for (i = 0; i < allocator->schedule_count; ++i)
        if (allocator->schedule[i].instruction == instruction) {
            *position = allocator->schedule[i].position;
            return 1;
        }
    return 0;
}

static enum RA_status RA_prepare_events(
        struct RA_allocator *allocator, struct RA_diagnostic *diagnostic) {
    size_t i, j;
    /*
     * Canonical RegMap events are allocation-ready and carry the exact
     * scheduled-instruction identity.  If they exist for a vreg, discard
     * fallback events inferred from MIR rather than duplicating or relocating
     * the authoritative facts.
     */
    for (i = 0; i < allocator->event_count;) {
        const struct RA_interval *interval =
            RA_find_interval(allocator, allocator->events[i].value);
        if (interval && interval->has_canonical_events &&
                !allocator->events[i].has_scheduled_instruction) {
            memmove(&allocator->events[i], &allocator->events[i + 1u],
                (allocator->event_count - i - 1u) * sizeof *allocator->events);
            --allocator->event_count;
        } else {
            ++i;
        }
    }
    for (i = 0; i < allocator->event_count; ++i) {
        struct RA_event *event = &allocator->events[i];
        uint32_t position;
        if (event->needs_schedule &&
                RA_schedule_position(allocator, event->point, &position))
            event->point = position;
        event->needs_schedule = 0u;
    }
    /* Deduplicate events after schedule ids have been resolved. */
    for (i = 0; i < allocator->event_count; ++i) {
        for (j = i + 1u; j < allocator->event_count;) {
            if (allocator->events[i].point == allocator->events[j].point &&
                    allocator->events[i].value == allocator->events[j].value &&
                    allocator->events[i].definition == allocator->events[j].definition) {
                if (!allocator->events[i].has_scheduled_instruction &&
                        allocator->events[j].has_scheduled_instruction) {
                    allocator->events[i].scheduled_instruction =
                        allocator->events[j].scheduled_instruction;
                    allocator->events[i].has_scheduled_instruction = 1u;
                } else if (allocator->events[i].has_scheduled_instruction &&
                        allocator->events[j].has_scheduled_instruction &&
                        allocator->events[i].scheduled_instruction !=
                            allocator->events[j].scheduled_instruction) {
                    return RA_diagnose(diagnostic, RA_INVALID_ARGUMENT, "", 0,
                        "conflicting scheduled instructions for one event");
                }
                memmove(&allocator->events[j], &allocator->events[j + 1u],
                    (allocator->event_count - j - 1u) * sizeof *allocator->events);
                --allocator->event_count;
            } else {
                ++j;
            }
        }
    }
    for (i = 0; i < allocator->interval_count; ++i) {
        struct RA_interval *interval = &allocator->intervals[i];
        uint64_t begin = UINT64_MAX, end = UINT64_C(0);
        if (interval->has_explicit_ranges)
            continue;
        for (j = 0; j < allocator->event_count; ++j) {
            const struct RA_event *event = &allocator->events[j];
            if (event->value != interval->value)
                continue;
            if (event->point < begin)
                begin = event->point;
            if (event->point == UINT64_MAX)
                return RA_diagnose(diagnostic, RA_INVALID_ARGUMENT, "", 0,
                    "an event at UINT64_MAX cannot form a half-open interval");
            if (event->point + UINT64_C(1) > end)
                end = event->point + UINT64_C(1);
        }
        if (begin != UINT64_MAX) {
            enum RA_status status = RA_add_range(allocator, interval->value, begin, end);
            if (status != RA_OK)
                return RA_diagnose(diagnostic, status, "", 0,
                    "cannot derive a live interval");
            interval = &allocator->intervals[i];
            interval->has_explicit_ranges = 0u;
        }
    }
    return RA_OK;
}

static uint64_t RA_first(const struct RA_interval *interval) {
    return interval->range_count ? interval->ranges[0].begin : UINT64_MAX;
}

static uint64_t RA_last(const struct RA_interval *interval) {
    return interval->range_count ?
        interval->ranges[interval->range_count - 1u].end : UINT64_C(0);
}

static int RA_interval_compare(const void *left, const void *right) {
    const struct RA_interval *a = (const struct RA_interval *)left;
    const struct RA_interval *b = (const struct RA_interval *)right;
    uint64_t a_start = RA_first(a), b_start = RA_first(b);
    if (a_start != b_start)
        return a_start < b_start ? -1 : 1;
    if (a->fixed != b->fixed)
        return a->fixed ? -1 : 1;
    if (a->external_value != b->external_value)
        return a->external_value < b->external_value ? -1 : 1;
    if (a->value != b->value)
        return a->value < b->value ? -1 : 1;
    return 0;
}

static int RA_register_compare(const void *left, const void *right) {
    const struct RA_register *a = (const struct RA_register *)left;
    const struct RA_register *b = (const struct RA_register *)right;
    if (a->canonical != b->canonical)
        return a->canonical < b->canonical ? -1 : 1;
    if (a->id != b->id)
        return a->id < b->id ? -1 : 1;
    return strcmp(a->name, b->name);
}

static int RA_ranges_overlap(
        const struct RA_interval *left, const struct RA_interval *right) {
    size_t i = 0, j = 0;
    while (i < left->range_count && j < right->range_count) {
        const struct RA_range *a = &left->ranges[i];
        const struct RA_range *b = &right->ranges[j];
        if (a->begin < b->end && b->begin < a->end)
            return 1;
        if (a->end <= b->begin)
            ++i;
        else
            ++j;
    }
    return 0;
}

static int RA_has_definition(
        const struct RA_allocator *allocator, uint32_t value) {
    size_t i;
    for (i = 0; i < allocator->event_count; ++i)
        if (allocator->events[i].value == value &&
                allocator->events[i].definition)
            return 1;
    return 0;
}

static int RA_register_available(
        const struct RA_allocator *allocator, size_t current,
        uint32_t physical, size_t excluded) {
    size_t i;
    for (i = 0; i < allocator->interval_count; ++i) {
        const struct RA_interval *other = &allocator->intervals[i];
        if (i == current || i == excluded || other->spilled ||
                other->assigned_register != physical)
            continue;
        if (RA_ranges_overlap(&allocator->intervals[current], other))
            return 0;
    }
    return 1;
}

static int RA_is_allocatable_register(
        const struct RA_allocator *allocator, size_t index) {
    const struct RA_register *row = &allocator->registers[index];
    size_t i;
    if (row->reserved || row->hardwired_zero || row->canonical != row->id)
        return 0;
    for (i = 0; i < index; ++i)
        if (allocator->registers[i].canonical == row->canonical)
            return 0;
    return 1;
}

enum RA_status RA_allocate(
        struct RA_allocator *allocator, struct RA_diagnostic *diagnostic) {
    size_t *active = NULL, active_count = 0, active_capacity = 0;
    size_t i, j;
    enum RA_status status;
    if (!allocator)
        return RA_diagnose(diagnostic, RA_INVALID_ARGUMENT, "", 0, "null allocator");
@ORACLE_ALLOCATE@
    status = RA_prepare_events(allocator, diagnostic);
    if (status != RA_OK)
        return status;
    for (i = 0; i < allocator->interval_count; ++i) {
        RA_normalize_ranges(&allocator->intervals[i]);
        allocator->intervals[i].assigned_register = RA_NO_REGISTER;
        allocator->intervals[i].spill_slot = RA_NO_SPILL_SLOT;
        allocator->intervals[i].spilled = 0u;
    }
    qsort(allocator->intervals, allocator->interval_count,
        sizeof *allocator->intervals, RA_interval_compare);
    qsort(allocator->registers, allocator->register_count,
        sizeof *allocator->registers, RA_register_compare);

    /* Precolor fixed intervals before ordinary scan decisions see them. */
    for (i = 0; i < allocator->interval_count; ++i) {
        struct RA_interval *interval = &allocator->intervals[i];
        const struct RA_register *physical;
        if (!interval->fixed)
            continue;
        physical = RA_const_register(allocator, interval->fixed_register);
        if (!physical)
            return RA_diagnose(diagnostic, RA_UNKNOWN_REGISTER, "", 0,
                "fixed interval names an unknown physical register");
        interval->fixed_register = physical->canonical;
        interval->assigned_register = physical->canonical;
        if (physical->hardwired_zero && RA_has_definition(allocator, interval->value))
            return RA_diagnose(diagnostic, RA_FIXED_ZERO_DEFINITION, "", 0,
                "a defining interval cannot be fixed to the hardwired zero register");
        for (j = 0; j < i; ++j) {
            const struct RA_interval *other = &allocator->intervals[j];
            if (other->fixed && other->assigned_register == interval->assigned_register &&
                    RA_ranges_overlap(interval, other))
                return RA_diagnose(diagnostic, RA_CONFLICTING_FIXED_INTERVALS, "", 0,
                    "overlapping fixed intervals require aliasing physical registers");
        }
    }

    for (i = 0; i < allocator->interval_count; ++i) {
        struct RA_interval *interval = &allocator->intervals[i];
        uint32_t chosen = RA_NO_REGISTER;
        size_t victim = SIZE_MAX;
        if (!interval->range_count || interval->fixed)
            continue;

        /* Expire active intervals whose final segment is before this start. */
        for (j = 0; j < active_count;) {
            if (RA_last(&allocator->intervals[active[j]]) <= RA_first(interval)) {
                memmove(&active[j], &active[j + 1u],
                    (active_count - j - 1u) * sizeof *active);
                --active_count;
            } else {
                ++j;
            }
        }

        for (j = 0; j < allocator->register_count; ++j) {
            uint32_t candidate;
            if (!RA_is_allocatable_register(allocator, j))
                continue;
            candidate = allocator->registers[j].canonical;
            if (RA_register_available(allocator, i, candidate, SIZE_MAX)) {
                chosen = candidate;
                break;
            }
        }

        if (chosen == RA_NO_REGISTER) {
            /*
             * Deterministic Poletto-Sarkar eviction: prefer spilling the
             * non-fixed active interval with the furthest final endpoint.
             */
            for (j = 0; j < active_count; ++j) {
                size_t candidate = active[j];
                struct RA_interval *other = &allocator->intervals[candidate];
                if (other->fixed || other->spilled ||
                        !RA_ranges_overlap(interval, other))
                    continue;
                if ((victim == SIZE_MAX ||
                        other->spill_cost <
                            allocator->intervals[victim].spill_cost ||
                        (other->spill_cost ==
                            allocator->intervals[victim].spill_cost &&
                         (RA_last(other) > RA_last(
                            &allocator->intervals[victim]) ||
                          (RA_last(other) ==
                                RA_last(&allocator->intervals[victim]) &&
                           other->external_value >
                                allocator->intervals[victim].external_value)))) &&
                        RA_register_available(allocator, i,
                            other->assigned_register, candidate))
                    victim = candidate;
            }
            if (victim != SIZE_MAX &&
                    (allocator->intervals[victim].spill_cost <
                        interval->spill_cost ||
                     (allocator->intervals[victim].spill_cost ==
                        interval->spill_cost &&
                      (RA_last(&allocator->intervals[victim]) >
                            RA_last(interval) ||
                       (RA_last(&allocator->intervals[victim]) ==
                            RA_last(interval) &&
                        allocator->intervals[victim].external_value >
                            interval->external_value))))) {
                chosen = allocator->intervals[victim].assigned_register;
                allocator->intervals[victim].assigned_register = RA_NO_REGISTER;
                allocator->intervals[victim].spilled = 1u;
                for (j = 0; j < active_count; ++j)
                    if (active[j] == victim) {
                        memmove(&active[j], &active[j + 1u],
                            (active_count - j - 1u) * sizeof *active);
                        --active_count;
                        break;
                    }
            }
        }

        if (chosen == RA_NO_REGISTER) {
            interval->spilled = 1u;
        } else {
            interval->assigned_register = chosen;
            if (!RA_grow_array((void **)&active, &active_capacity, active_count,
                    sizeof *active)) {
                free(active);
                return RA_diagnose(diagnostic, RA_OUT_OF_MEMORY, "", 0,
                    "cannot grow the active interval set");
            }
            active[active_count++] = i;
        }
    }
    free(active);

    allocator->spill_count = 0;
    for (i = 0; i < allocator->interval_count; ++i)
        if (allocator->intervals[i].spilled)
            allocator->intervals[i].spill_slot = allocator->spill_count++;
    allocator->allocated = 1u;
    return RA_diagnose(diagnostic, RA_OK, "", 0, "ok");
}

const struct RA_interval *RA_find_interval(
        const struct RA_allocator *allocator, uint32_t value) {
    size_t i;
    if (!allocator)
        return NULL;
    for (i = 0; i < allocator->interval_count; ++i)
        if (allocator->intervals[i].value == value)
            return &allocator->intervals[i];
    return NULL;
}

const char *RA_register_name(
        const struct RA_allocator *allocator, uint32_t canonical) {
    size_t i;
    if (!allocator)
        return NULL;
    for (i = 0; i < allocator->register_count; ++i)
        if (allocator->registers[i].id == canonical)
            return allocator->registers[i].name;
    for (i = 0; i < allocator->register_count; ++i)
        if (allocator->registers[i].canonical == canonical)
            return allocator->registers[i].name;
    return NULL;
}

static int RA_write_string(FILE *stream, const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    if (fputc('"', stream) == EOF)
        return 0;
    while (*cursor) {
        if (*cursor == '"' || *cursor == '\\') {
            if (fputc('\\', stream) == EOF || fputc(*cursor, stream) == EOF)
                return 0;
        } else if (*cursor >= 0x20u && *cursor <= 0x7eu &&
                *cursor != (unsigned char)'?') {
            if (fputc(*cursor, stream) == EOF)
                return 0;
        } else if (fprintf(stream, "\\%03o", (unsigned)*cursor) < 0) {
            return 0;
        }
        ++cursor;
    }
    return fputc('"', stream) != EOF;
}

static int RA_event_compare(const void *left, const void *right) {
    const struct RA_event *a = (const struct RA_event *)left;
    const struct RA_event *b = (const struct RA_event *)right;
    if (a->point != b->point)
        return a->point < b->point ? -1 : 1;
    if (a->definition != b->definition)
        return a->definition ? -1 : 1;
    if (a->value != b->value)
        return a->value < b->value ? -1 : 1;
    if (a->has_scheduled_instruction != b->has_scheduled_instruction)
        return a->has_scheduled_instruction ? -1 : 1;
    if (a->scheduled_instruction != b->scheduled_instruction)
        return a->scheduled_instruction < b->scheduled_instruction ? -1 : 1;
    return 0;
}

static int RA_alias_compare(const void *left, const void *right) {
    const struct RA_alias *a = (const struct RA_alias *)left;
    const struct RA_alias *b = (const struct RA_alias *)right;
    int order = strcmp(a->name, b->name);
    if (order)
        return order;
    if (a->canonical != b->canonical)
        return a->canonical < b->canonical ? -1 : 1;
    return 0;
}

static uint64_t RA_hash_u64(uint64_t hash, uint64_t value) {
    unsigned i;
    for (i = 0; i < 8u; ++i) {
        hash ^= value & UINT64_C(0xff);
        hash *= UINT64_C(1099511628211);
        value >>= 8;
    }
    return hash;
}

static uint64_t RA_output_stream_id(const struct RA_allocator *allocator) {
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;
    hash = RA_hash_u64(hash, allocator->scheduled_stream_id);
    hash = RA_hash_u64(hash, allocator->reg_map_stream_id);
    for (i = 0; i < allocator->interval_count; ++i) {
        const struct RA_interval *interval = &allocator->intervals[i];
        size_t j;
        hash = RA_hash_u64(hash, interval->external_value);
        hash = RA_hash_u64(hash, interval->width);
        hash = RA_hash_u64(hash, interval->assigned_register);
        hash = RA_hash_u64(hash, interval->spill_slot);
        hash = RA_hash_u64(hash, interval->range_count);
        for (j = 0; j < interval->range_count; ++j) {
            hash = RA_hash_u64(hash, interval->ranges[j].begin);
            hash = RA_hash_u64(hash, interval->ranges[j].end);
        }
    }
    for (i = 0; i < allocator->event_count; ++i) {
        const struct RA_event *event = &allocator->events[i];
        hash = RA_hash_u64(hash, event->point);
        hash = RA_hash_u64(hash, event->value);
        hash = RA_hash_u64(hash, event->definition);
        hash = RA_hash_u64(hash, event->has_scheduled_instruction ?
            event->scheduled_instruction : UINT64_C(0));
    }
    return hash ? hash : UINT64_C(1);
}

static uint64_t RA_scheduled_instruction_at(
        const struct RA_allocator *allocator, uint64_t position) {
    size_t i;
    for (i = 0; i < allocator->schedule_count; ++i)
        if (allocator->schedule[i].position == position)
            return allocator->schedule[i].external ?
                allocator->schedule[i].external_scheduled_instruction :
                allocator->schedule[i].scheduled_instruction;
    return position == UINT64_MAX ? UINT64_MAX : position + UINT64_C(1);
}

static uint64_t RA_external_register_id(
        const struct RA_allocator *allocator, uint32_t canonical) {
    size_t i;
    for (i = 0; i < allocator->register_count; ++i)
        if (allocator->registers[i].id == canonical)
            return allocator->registers[i].external_canonical ?
                allocator->registers[i].external_canonical : canonical;
    return canonical;
}

enum RA_status RA_write(
        FILE *stream, const struct RA_allocator *allocator,
        struct RA_diagnostic *diagnostic) {
    struct RA_event *events = NULL;
    struct RA_alias *aliases = NULL;
    size_t i, j, assignment_count = 0, event_count = 0;
    uint64_t stream_id, scheduled_stream_id, reg_map_stream_id;
    if (!stream || !allocator || !allocator->allocated)
        return RA_diagnose(diagnostic, RA_INVALID_ARGUMENT, "", 0,
            "allocator has not produced an allocation");
    if (allocator->event_count) {
        events = (struct RA_event *)malloc(
            allocator->event_count * sizeof *events);
        if (!events)
            return RA_diagnose(diagnostic, RA_OUT_OF_MEMORY, "", 0,
                "cannot sort allocation events");
        memcpy(events, allocator->events, allocator->event_count * sizeof *events);
        qsort(events, allocator->event_count, sizeof *events, RA_event_compare);
    }
    if (allocator->alias_count) {
        aliases = (struct RA_alias *)malloc(
            allocator->alias_count * sizeof *aliases);
        if (!aliases) {
            free(events);
            return RA_diagnose(diagnostic, RA_OUT_OF_MEMORY, "", 0,
                "cannot sort register aliases");
        }
        memcpy(aliases, allocator->aliases, allocator->alias_count * sizeof *aliases);
        qsort(aliases, allocator->alias_count, sizeof *aliases, RA_alias_compare);
    }

    for (i = 0; i < allocator->interval_count; ++i)
        if (allocator->intervals[i].spilled ||
                allocator->intervals[i].assigned_register != RA_NO_REGISTER)
            ++assignment_count;
    for (i = 0; i < allocator->event_count; ++i) {
        const struct RA_interval *interval =
            RA_find_interval(allocator, events[i].value);
        if (interval && (interval->spilled ||
                interval->assigned_register != RA_NO_REGISTER))
            ++event_count;
    }
    stream_id = RA_output_stream_id(allocator);
    scheduled_stream_id = allocator->scheduled_stream_id;
    reg_map_stream_id = allocator->reg_map_stream_id;

    if (fprintf(stream,
            "/* Generated by Archon Regalloc; canonical C99 X-macro stream. */\n"
            "#include \"MacroABI.h\"\n"
            "\n"
            "#if ARCHON_MACRO_ABI_VERSION != 0x00010000UL\n"
            "# error \"Regallocated.i requires Archon Macro ABI 1.0.0\"\n"
            "#endif\n"
            "#if ARCHON_REGALLOCATED_SCHEMA_VERSION != 1UL\n"
            "# error \"Regallocated.i schema version is incompatible with MacroABI.h\"\n"
            "#endif\n"
            "#ifdef ARCHON_MACRO_ABI_ACTIVE_REGALLOCATED\n"
            "# error \"recursive inclusion of Regallocated.i\"\n"
            "#endif\n"
            "#define ARCHON_MACRO_ABI_ACTIVE_REGALLOCATED 1\n"
            "\n"
            "#ifndef ARCHON_REGALLOCATED_BEGIN\n"
            "# define ARCHON_REGALLOCATED_BEGIN(A0,A1,A2,A3,A4,A5,A6)\n"
            "# define ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_BEGIN 1\n"
            "#endif\n"
            "#ifndef ARCHON_REGALLOCATED_ASSIGN\n"
            "# define ARCHON_REGALLOCATED_ASSIGN(A0,A1,A2,A3,A4,A5,A6,A7)\n"
            "# define ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_ASSIGN 1\n"
            "#endif\n"
            "#ifndef ARCHON_REGALLOCATED_EVENT\n"
            "# define ARCHON_REGALLOCATED_EVENT(A0,A1,A2,A3,A4,A5,A6,A7)\n"
            "# define ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_EVENT 1\n"
            "#endif\n"
            "#ifndef ARCHON_REGALLOCATED_END\n"
            "# define ARCHON_REGALLOCATED_END(A0)\n"
            "# define ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_END 1\n"
            "#endif\n\n"
            "ARCHON_REGALLOCATED_BEGIN(ARCHON_MACRO_ABI_VERSION, "
            "ARCHON_REGALLOCATED_SCHEMA_VERSION, UINT64_C(0x%016" PRIX64
            "), UINT64_C(0x%016" PRIX64 "), UINT64_C(0x%016" PRIX64
            "), UINT32_C(%zu), UINT32_C(%zu))\n",
            stream_id, scheduled_stream_id, reg_map_stream_id,
            assignment_count, event_count) < 0)
        goto io_failure;
    j = 0;
    for (i = 0; i < allocator->interval_count; ++i) {
        const struct RA_interval *interval = &allocator->intervals[i];
        if (!interval->spilled &&
                interval->assigned_register == RA_NO_REGISTER)
            continue;
        if (interval->spilled) {
            if (fprintf(stream,
                    "ARCHON_REGALLOCATED_ASSIGN(UINT64_C(0x%016" PRIX64
                    "), UINT32_C(%zu), UINT64_C(0x%016" PRIX64
                    "), ARCHON_LOCATION_STACK, UINT64_C(0x0000000000000000), "
                    "INT64_C(%" PRIu32 "), UINT32_C(%" PRIu32 "), UINT32_C(0))\n",
                    (uint64_t)j + UINT64_C(1), j, interval->external_value,
                    interval->spill_slot,
                    interval->width) < 0)
                goto io_failure;
        } else if (fprintf(stream,
                "ARCHON_REGALLOCATED_ASSIGN(UINT64_C(0x%016" PRIX64
                "), UINT32_C(%zu), UINT64_C(0x%016" PRIX64
                "), ARCHON_LOCATION_REGISTER, UINT64_C(0x%016" PRIX64
                "), (-INT64_C(1)), UINT32_C(%" PRIu32
                "), UINT32_C(0))\n",
                (uint64_t)j + UINT64_C(1), j, interval->external_value,
                RA_external_register_id(
                    allocator, interval->assigned_register),
                interval->width) < 0) {
            goto io_failure;
        }
        ++j;
    }
    j = 0;
    for (i = 0; i < allocator->event_count; ++i) {
        const struct RA_event *event = &events[i];
        const struct RA_interval *interval =
            RA_find_interval(allocator, event->value);
        const char *kind;
        char spill[64];
        uint64_t scheduled_id;
        if (!interval || (!interval->spilled &&
                interval->assigned_register == RA_NO_REGISTER))
            continue;
        kind = interval->spilled ?
            (event->definition ? "ARCHON_EVENT_SPILL" : "ARCHON_EVENT_RELOAD") :
            (event->definition ? "ARCHON_EVENT_DEF" : "ARCHON_EVENT_USE");
        if (interval->spilled)
            (void)snprintf(spill, sizeof spill, "INT64_C(%" PRIu32 ")",
                interval->spill_slot);
        else
            (void)snprintf(spill, sizeof spill, "(-INT64_C(1))");
        scheduled_id = event->has_scheduled_instruction ?
            event->scheduled_instruction :
            RA_scheduled_instruction_at(allocator, event->point);
        if (scheduled_id == UINT64_C(0))
            scheduled_id = UINT64_C(1);
        if (fprintf(stream,
                "ARCHON_REGALLOCATED_EVENT(UINT64_C(0x%016" PRIX64
                "), UINT32_C(%zu), UINT64_C(0x%016" PRIX64
                "), %s, UINT64_C(0x%016" PRIX64
                "), UINT64_C(0x%016" PRIX64 "), "
                "%s"
                ", UINT32_C(0))\n",
                (uint64_t)j + UINT64_C(1), j, scheduled_id,
                kind, interval->external_value,
                interval->spilled ? UINT64_C(0) :
                    RA_external_register_id(
                        allocator, interval->assigned_register),
                spill) < 0)
            goto io_failure;
        ++j;
    }
    if (fprintf(stream,
            "ARCHON_REGALLOCATED_END(UINT64_C(0x%016" PRIX64 "))\n\n"
            "#ifdef ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_END\n"
            "# undef ARCHON_REGALLOCATED_END\n"
            "# undef ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_END\n"
            "#endif\n"
            "#ifdef ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_EVENT\n"
            "# undef ARCHON_REGALLOCATED_EVENT\n"
            "# undef ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_EVENT\n"
            "#endif\n"
            "#ifdef ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_ASSIGN\n"
            "# undef ARCHON_REGALLOCATED_ASSIGN\n"
            "# undef ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_ASSIGN\n"
            "#endif\n"
            "#ifdef ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_BEGIN\n"
            "# undef ARCHON_REGALLOCATED_BEGIN\n"
            "# undef ARCHON_MACRO_ABI_LOCAL_REGALLOCATED_BEGIN\n"
            "#endif\n"
            "#undef ARCHON_MACRO_ABI_ACTIVE_REGALLOCATED\n\n",
            stream_id) < 0)
        goto io_failure;

    /* Compatibility facts remain non-ARCHON callbacks and are ignored by ABI readers. */
    if (fprintf(stream,
            "/* Compatibility allocation facts; abstract spills are intentional. */\n"
            "#ifdef ARCHON_REGALLOC_LEGACY_CALLBACKS\n"
            "REGALLOC_BEGIN(%zu, %" PRIu32 ")\n",
            allocator->interval_count, allocator->spill_count) < 0)
        goto io_failure;
    for (i = 0; i < allocator->register_count; ++i) {
        const struct RA_register *row = &allocator->registers[i];
        if (row->canonical != row->id) {
            if (fprintf(stream, "REGALLOC_ALIAS_GROUP(%" PRIu32
                    ", %" PRIu32 ")\n", row->id, row->canonical) < 0)
                goto io_failure;
            continue;
        }
        if (fprintf(stream, "REGALLOC_REGISTER(%" PRIu32 ", ", row->id) < 0 ||
                !RA_write_string(stream, row->name) ||
                fputs(")\n", stream) == EOF)
            goto io_failure;
        if (row->reserved &&
                fprintf(stream, "REGALLOC_RESERVED(%" PRIu32 ")\n", row->id) < 0)
            goto io_failure;
        if (row->hardwired_zero &&
                fprintf(stream, "REGALLOC_ZERO(%" PRIu32 ")\n", row->id) < 0)
            goto io_failure;
    }
    for (i = 0; i < allocator->alias_count; ++i) {
        if (fputs("REGALLOC_ALIAS(", stream) == EOF ||
                !RA_write_string(stream, aliases[i].name) ||
                fprintf(stream, ", %" PRIu32 ")\n", aliases[i].canonical) < 0)
            goto io_failure;
    }
    for (i = 0; i < allocator->interval_count; ++i) {
        const struct RA_interval *interval = &allocator->intervals[i];
        for (j = 0; j < interval->range_count; ++j)
            if (fprintf(stream, "REGALLOC_RANGE(%" PRIu32 ", %" PRIu64
                    ", %" PRIu64 ")\n", interval->value,
                    interval->ranges[j].begin, interval->ranges[j].end) < 0)
                goto io_failure;
        if (interval->fixed &&
                fprintf(stream, "REGALLOC_FIXED(%" PRIu32 ", %" PRIu32 ")\n",
                    interval->value, interval->fixed_register) < 0)
            goto io_failure;
        if (interval->spilled) {
            if (fprintf(stream, "REGALLOC_SPILL_SLOT(%" PRIu32 ", %" PRIu32 ")\n"
                    "REGALLOC_ABSTRACT_SPILL(%" PRIu32 ", %" PRIu32 ")\n",
                    interval->value, interval->spill_slot,
                    interval->value, interval->spill_slot) < 0)
                goto io_failure;
        } else if (interval->assigned_register != RA_NO_REGISTER) {
            if (fprintf(stream, "REGALLOC_ASSIGN(%" PRIu32 ", %" PRIu32 ")\n",
                    interval->value, interval->assigned_register) < 0)
                goto io_failure;
        }
    }
    for (i = 0; i < allocator->event_count; ++i) {
        const struct RA_event *event = &events[i];
        const struct RA_interval *interval = RA_find_interval(allocator, event->value);
        if (!interval)
            continue;
        if (interval->spilled) {
            const char *kind = event->definition ?
                "REGALLOC_SPILL" : "REGALLOC_RELOAD";
            if (fprintf(stream, "%s(%" PRIu64 ", %" PRIu32 ", %" PRIu32 ")\n",
                    kind, event->point, event->value, interval->spill_slot) < 0)
                goto io_failure;
        } else if (interval->assigned_register != RA_NO_REGISTER) {
            const char *kind = event->definition ?
                "REGALLOC_DEF" : "REGALLOC_USE";
            if (fprintf(stream, "%s(%" PRIu64 ", %" PRIu32 ", %" PRIu32 ")\n",
                    kind, event->point, event->value,
                    interval->assigned_register) < 0)
                goto io_failure;
        }
    }
    if (fputs("REGALLOC_END()\n#endif\n", stream) == EOF ||
            fflush(stream) == EOF)
        goto io_failure;
    free(events);
    free(aliases);
    return RA_diagnose(diagnostic, RA_OK, "", 0, "ok");

io_failure:
    free(events);
    free(aliases);
    return RA_diagnose(diagnostic, RA_IO_ERROR, "", 0,
        "cannot write Regallocated.i");
}

static enum RA_status RA_open_and_load(
        struct RA_allocator *allocator, const char *path,
        enum RA_stream_kind kind, struct RA_diagnostic *diagnostic) {
    FILE *stream;
    enum RA_status status;
    if (!path)
        return RA_OK;
    stream = fopen(path, "r");
    if (!stream)
        return RA_diagnose(diagnostic, RA_IO_ERROR, path, 0,
            "cannot open macro stream");
    status = RA_load_stream(allocator, stream, path, kind, diagnostic);
    if (fclose(stream) != 0 && status == RA_OK)
        status = RA_diagnose(diagnostic, RA_IO_ERROR, path, 0,
            "cannot close macro stream");
    return status;
}

enum RA_status RA_run_files(
        const char *scheduled_path, const char *mir_path,
        const char *regmap_path, const char *output_path,
        struct RA_diagnostic *diagnostic) {
    struct RA_allocator allocator;
    enum RA_status status;
    FILE *output;
    int output_created = 0;
    if (!scheduled_path || !mir_path || !output_path)
        return RA_diagnose(diagnostic, RA_INVALID_ARGUMENT, "", 0,
            "scheduled, MIR, and output paths are required");
    RA_init(&allocator);
@ORACLE_LOAD_REGMAP@
    if (status == RA_OK)
        status = RA_open_and_load(
            &allocator, scheduled_path, RA_STREAM_SCHEDULED, diagnostic);
    if (status == RA_OK)
        status = RA_open_and_load(&allocator, mir_path, RA_STREAM_MIR, diagnostic);
@ORACLE_RUN_FILES@
    if (status == RA_OK)
        status = RA_allocate(&allocator, diagnostic);
    if (status == RA_OK) {
        output = fopen(output_path, "w");
        if (!output) {
            status = RA_diagnose(diagnostic, RA_IO_ERROR, output_path, 0,
                "cannot create Regallocated.i");
        } else {
            output_created = 1;
            status = RA_write(output, &allocator, diagnostic);
            if (fclose(output) != 0 && status == RA_OK)
                status = RA_diagnose(diagnostic, RA_IO_ERROR, output_path, 0,
                    "cannot close Regallocated.i");
        }
    }
    if (status != RA_OK && output_created)
        (void)remove(output_path);
    RA_destroy(&allocator);
    return status;
}

const char *RA_status_string(enum RA_status status) {
    static const char *const names[] = {
        "ok", "invalid argument", "out of memory", "parse error",
        "unknown register", "conflicting fixed intervals",
        "definition fixed to hardwired zero", "Oracle error", "I/O error"
    };
    if (status < RA_OK || status > RA_IO_ERROR)
        return "unknown register-allocation status";
    return names[(unsigned)status];
}

#ifndef ARCHON_REGALLOC_NO_MAIN
int main(int argc, char **argv) {
    const char *scheduled = "Scheduled.i";
    const char *mir = "MIR.i";
    const char *regmap = "RegMap.i";
    const char *output = "Regallocated.i";
    struct RA_diagnostic diagnostic;
    enum RA_status status;
    if (argc > 5) {
        (void)fprintf(stderr,
            "usage: %s [Scheduled.i [MIR.i [RegMap.i [Regallocated.i]]]]\n",
            argv[0]);
        return 2;
    }
    if (argc > 1)
        scheduled = argv[1];
    if (argc > 2)
        mir = argv[2];
    if (argc > 3)
        regmap = strcmp(argv[3], "-") == 0 ? NULL : argv[3];
    if (argc > 4)
        output = argv[4];
    memset(&diagnostic, 0, sizeof diagnostic);
    status = RA_run_files(scheduled, mir, regmap, output, &diagnostic);
    if (status != RA_OK) {
        if (diagnostic.source[0])
            (void)fprintf(stderr, "%s:%zu: ", diagnostic.source, diagnostic.line);
        (void)fprintf(stderr, "regalloc: %s: %s\n",
            RA_status_string(status), diagnostic.message);
        return 1;
    }
    return 0;
}
#endif
"""
