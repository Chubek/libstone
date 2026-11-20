"""Generate the representation adapter and linked C99 pipeline runtime.

The individual IR libraries intentionally keep their native data models.  This
stage supplies a narrow adapter into one conservative machine-pipeline model so
the final driver never casts one representation's ``IR.h`` as another.  Only
operations whose meaning is explicit in the source IR, or explicit target
instruction opcodes, enter instruction selection.  Everything else is retained
as an unsupported barrier and diagnosed before code emission.
"""

from __future__ import annotations

from collections.abc import Mapping

from ..diagnostics import ArchonError
from .emitter import _is_stack_target
from .macros import macro_abi_header
from .sea_of_nodes import _hash as _sea_opcode


NAME = "pipeline"
ALIASES = ("linked-pipeline", "representation-adapter")
STAGE_ORDER = 650


def _database(context: object) -> Mapping:
    if isinstance(context, Mapping):
        for key in ("database", "db", "archdb"):
            value = context.get(key)
            if isinstance(value, Mapping):
                return value
        for key in ("architectures", "databases", "archdbs", "targets"):
            value = context.get(key)
            if isinstance(value, (list, tuple)) and value:
                first = value[0]
                if isinstance(first, Mapping):
                    return first
    raise ArchonError("the linked pipeline requires at least one compiled ArchDB")


def _c_string(value: object) -> str:
    pieces = ['"']
    for byte in str(value).encode("utf-8"):
        if byte == 0x22:
            pieces.append(r"\"")
        elif byte == 0x5C:
            pieces.append(r"\\")
        elif 0x20 <= byte <= 0x7E and byte != 0x3F:
            pieces.append(chr(byte))
        else:
            pieces.append(f"\\{byte:03o}")
    pieces.append('"')
    return "".join(pieces)


def _representation(context: object) -> str:
    value = context.get("ir") if isinstance(context, Mapping) else None
    if not isinstance(value, str):
        return "anf"
    return value.strip().lower().replace("_", "-")


def _oracle_enabled(context: object) -> bool:
    if not isinstance(context, Mapping):
        return True
    options = context.get("options", {})
    if options is None:
        options = {}
    if not isinstance(options, Mapping):
        raise ArchonError("linked-pipeline options must be an object")
    enabled = options.get("oracle", True)
    if type(enabled) is not bool:
        raise ArchonError("linked-pipeline option 'oracle' must be boolean")
    return enabled


def _word_width(db: Mapping) -> int:
    registers = db.get("registers")
    if isinstance(registers, Mapping):
        width = registers.get("width")
        if type(width) is int and width > 0:
            return width
    width = db.get("address_width")
    return width if type(width) is int and width > 0 else 64


def _sea_targets(db: Mapping) -> str:
    architecture = str(db.get("architecture", "architecture"))
    rows = []
    for instruction in sorted(
        (
            row
            for row in db.get("instructions", ())
            if isinstance(row, Mapping) and isinstance(row.get("name"), str)
        ),
        key=lambda row: str(row["name"]),
    ):
        opcode = _sea_opcode(f"target:{architecture}:{instruction['name']}")
        rows.append(
            "    {UINT32_C(0x%08x), %s}," %
            (opcode, _c_string(instruction["name"]))
        )
    return "\n".join(rows) or "    {UINT32_C(0), NULL},"


def _adapter(representation: str) -> str:
    try:
        return _ADAPTERS[representation]
    except KeyError as exc:
        raise ArchonError(
            f"linked pipeline has no adapter for IR representation "
            f"{representation!r}"
        ) from exc


def emit(context: object) -> dict[str, str]:
    db = _database(context)
    representation = _representation(context)
    oracle_enabled = _oracle_enabled(context)
    oracle_include = '#include "Oracle.h"\n' if oracle_enabled else ""
    oracle_option_field = (
        "    const ArchonOracleOptions *oracle;\n" if oracle_enabled else ""
    )
    oracle_context_field = (
        "    const ArchonOracleOptions *oracle_options;\n"
        "    ArchonOracleAnnotations isel_annotations;\n"
        "    ArchonOracleAnnotations isched_annotations;\n"
        "    int isel_annotations_loaded;\n"
        "    int isched_annotations_loaded;\n"
        "    int *isched_priorities;\n"
        "    unsigned char *isched_priority_known;\n"
        "    ISched_KnownFacts isched_known;\n"
        if oracle_enabled
        else ""
    )
    oracle_context_init = (
        "    context.oracle_options = options->oracle;\n"
        if oracle_enabled
        else ""
    )
    oracle_emitter_local = (
        "        Emitter_Options emitter_options = *options->emitter;\n"
        "        if (emitter_options.oracle == NULL)\n"
        "            emitter_options.oracle = options->oracle;\n"
        if oracle_enabled
        else ""
    )
    oracle_emitter_argument = (
        "&emitter_options" if oracle_enabled else "options->emitter"
    )
    oracle_annotation_context_init = (
        "    archon_oracle_annotations_init(&context->isel_annotations);\n"
        "    archon_oracle_annotations_init(&context->isched_annotations);\n"
        if oracle_enabled
        else ""
    )
    oracle_annotation_context_dispose = (
        "    archon_oracle_annotations_dispose(&context->isel_annotations);\n"
        "    archon_oracle_annotations_dispose(&context->isched_annotations);\n"
        "    free(context->isched_priorities);\n"
        "    free(context->isched_priority_known);\n"
        if oracle_enabled
        else ""
    )
    oracle_support = _ORACLE_SUPPORT if oracle_enabled else ""
    oracle_rewrite = (
        "    if (!AP_oracle_process(\n"
        '            context, "Rewritten.i", ARCHON_ORACLE_STAGE_REWRITE)) {\n'
        '        (void)remove("Rewritten.i");\n'
        "        return 0;\n"
        "    }\n"
        if oracle_enabled
        else ""
    )
    oracle_rewrite_prepare = (
        "    char oracle_annotation_path[\n"
        "        ARCHON_ORACLE_MAX_PATH_BYTES + 1u];\n"
        "    const char *rewrite_advice_path;\n"
        "    if (!AP_oracle_annotation_path(\n"
        "            context, \"Rewritten.i\", oracle_annotation_path,\n"
        "            sizeof oracle_annotation_path))\n"
        "        return 0;\n"
        "    rewrite_advice_path = AP_oracle_active(context)\n"
        "        ? NULL : oracle_annotation_path;\n"
        if oracle_enabled
        else "    const char *rewrite_advice_path = NULL;\n"
    )
    oracle_rewrite_active_pass = (
        "    if (AP_oracle_active(context)) {\n"
        "        if (!AP_run_rewrite_engine(\n"
        "                context, engine, oracle_annotation_path, &summary)) {\n"
        "            (void)remove(\"Rewritten.i\");\n"
        "            return 0;\n"
        "        }\n"
        "        if (!AP_write_rewrite_artifact(context, &summary))\n"
        "            return 0;\n"
        "    }\n"
        if oracle_enabled
        else ""
    )
    oracle_mir = (
        "    /*\n"
        "     * SCHEDULE annotations are the closed MIR boundary output.  A\n"
        "     * continued linked run requests and consumes a fresh ISEL\n"
        "     * sidecar before making selection decisions.\n"
        "     */\n"
        "    if (success && !AP_oracle_process(\n"
        '            context, "MIR.i", ARCHON_ORACLE_STAGE_SCHEDULE))\n'
        "        success = 0;\n"
        if oracle_enabled
        else ""
    )
    oracle_scheduled = (
        "    /*\n"
        "     * This is an output annotation for the committed ISCHED\n"
        "     * artifact and for restart/downstream consumers; it is never\n"
        "     * applied post hoc to the schedule that produced it.\n"
        "     */\n"
        "    if (!AP_oracle_process(\n"
        '            context, "Scheduled.i", ARCHON_ORACLE_STAGE_ISCHED)) {\n'
        '        (void)remove("Scheduled.i");\n'
        "        return 0;\n"
        "    }\n"
        if oracle_enabled
        else ""
    )
    oracle_select_prepare = (
        "    if (!AP_oracle_process(\n"
        "            context, \"MIR.i\", ARCHON_ORACLE_STAGE_ISEL) ||\n"
        "            !AP_oracle_load_annotations(\n"
        "                context, \"MIR.i\", ARCHON_ORACLE_STAGE_ISEL,\n"
        "                &context->isel_annotations,\n"
        "                &context->isel_annotations_loaded)) {\n"
        "        (void)remove(\"MIR.i\");\n"
        "        return 0;\n"
        "    }\n"
        if oracle_enabled
        else ""
    )
    oracle_select_call = (
        "        status = context->isel_annotations_loaded\n"
        "            ? archon_isel_select_operation_with_annotations(\n"
        "                Emitter_target_name(), &request,\n"
        "                &context->isel_annotations, &choice,\n"
        "                message, sizeof message)\n"
        "            : archon_isel_select_operation(\n"
        "                Emitter_target_name(), &request, &choice,\n"
        "                message, sizeof message);\n"
        if oracle_enabled
        else (
            "        status = archon_isel_select_operation(\n"
            "            Emitter_target_name(), &request, &choice,\n"
            "            message, sizeof message);\n"
        )
    )
    oracle_isched_prepare = (
        "    if (!AP_oracle_process(\n"
        "            context, \"InstrMap.i\", ARCHON_ORACLE_STAGE_ISCHED) ||\n"
        "            !AP_oracle_load_annotations(\n"
        "                context, \"InstrMap.i\", ARCHON_ORACLE_STAGE_ISCHED,\n"
        "                &context->isched_annotations,\n"
        "                &context->isched_annotations_loaded)) {\n"
        "        (void)remove(\"InstrMap.i\");\n"
        "        (void)remove(\"Selected.i\");\n"
        "        return 0;\n"
        "    }\n"
        "    if (context->isched_annotations_loaded) {\n"
        "        size_t oracle_operation;\n"
        "        context->isched_priorities = (int *)calloc(\n"
        "            context->selected_count != 0u\n"
        "                ? context->selected_count : 1u,\n"
        "            sizeof *context->isched_priorities);\n"
        "        context->isched_priority_known =\n"
        "            (unsigned char *)malloc(\n"
        "                context->selected_count != 0u\n"
        "                    ? context->selected_count : 1u);\n"
        "        if (context->isched_priorities == NULL ||\n"
        "                context->isched_priority_known == NULL)\n"
        "            return AP_fail(\n"
        "                context, \"out of memory preparing ISCHED advice\");\n"
        "        for (oracle_operation = 0u;\n"
        "             oracle_operation < context->selected_count;\n"
        "             ++oracle_operation)\n"
        "            context->isched_priority_known[oracle_operation] = 1u;\n"
        "        memset(&context->isched_known, 0,\n"
        "               sizeof context->isched_known);\n"
        "        context->isched_known.operation_count =\n"
        "            context->selected_count;\n"
        "        context->isched_known.resource_count = 1u;\n"
        "        context->isched_known.operation_priority =\n"
        "            context->isched_priorities;\n"
        "        context->isched_known.operation_priority_known =\n"
        "            context->isched_priority_known;\n"
        "    }\n"
        if oracle_enabled
        else ""
    )
    oracle_isched_call = (
        "    if (context->isched_annotations_loaded) {\n"
        "        char oracle_message[160];\n"
        "        oracle_message[0] = '\\0';\n"
        "        status = isched_schedule_with_annotations(\n"
        "            &context->problem, &options, &context->isched_known,\n"
        "            &context->isched_annotations, &context->schedule,\n"
        "            oracle_message, sizeof oracle_message);\n"
        "        if (status == ISCHED_STATUS_ORACLE_ERROR)\n"
        "            return AP_fail(\n"
        "                context, \"instruction-scheduler Oracle advice "
        "failed: %s\",\n"
        "                oracle_message[0] != '\\0'\n"
        "                    ? oracle_message : \"invalid annotation\");\n"
        "    } else {\n"
        "        status = isched_schedule(\n"
        "            &context->problem, &options, &context->schedule);\n"
        "    }\n"
        if oracle_enabled
        else (
            "    status = isched_schedule(\n"
            "        &context->problem, &options, &context->schedule);\n"
        )
    )
    oracle_regmap = (
        "    if (!AP_oracle_process_default_sidecar(\n"
        "            &context, \"RegMap.i\", "
        "ARCHON_ORACLE_STAGE_REGALLOC)) {\n"
        "        (void)remove(\"RegMap.i\");\n"
        "        success = 0;\n"
        "        goto done;\n"
        "    }\n"
        if oracle_enabled
        else ""
    )
    oracle_rewrite_locals = (
        "    ArchonRewriteOracleConfig oracle_config;\n"
        "    ArchonOracleDiagnostic oracle_diagnostic;\n"
        "    ArchonOracleStatus oracle_status;\n"
        "    archon_rewrite_oracle_config_init(&oracle_config);\n"
        if oracle_enabled
        else ""
    )
    oracle_rewrite_load = (
        "    if (annotation_path != NULL) {\n"
        "        oracle_status = archon_rewrite_oracle_config_load(\n"
        "            annotation_path,\n"
        "            engine == ARCHON_PIPELINE_REWRITE_EGRAPH\n"
        "                ? ARCHON_REWRITE_EQUALITY_SATURATION\n"
        "                : ARCHON_REWRITE_DIRECTED,\n"
        "            max_iterations, &oracle_config, &oracle_diagnostic);\n"
        "        if (oracle_status != ARCHON_ORACLE_STATUS_OK) {\n"
        "            (void)AP_fail(\n"
        "                context, \"invalid rewrite Oracle sidecar: %s%s%s\",\n"
        "                archon_oracle_status_string(oracle_status),\n"
        "                oracle_diagnostic.message[0] != '\\0'\n"
        "                    ? \": \" : \"\",\n"
        "                oracle_diagnostic.message);\n"
        "            success = 0;\n"
        "            goto rewrite_done;\n"
        "        }\n"
        "        enabled = oracle_config.enabled;\n"
        "        max_iterations = oracle_config.max_iterations;\n"
        "        engine = oracle_config.engine ==\n"
        "                ARCHON_REWRITE_EQUALITY_SATURATION\n"
        "            ? ARCHON_PIPELINE_REWRITE_EGRAPH\n"
        "            : ARCHON_PIPELINE_REWRITE_CONVENTIONAL;\n"
        "    }\n"
        if oracle_enabled
        else ""
    )
    oracle_rewrite_operation = (
        "        int operation_forbidden;\n"
        "        int64_t operation_priority;\n"
        "        oracle_status = archon_rewrite_oracle_operation(\n"
        "            &oracle_config,\n"
        "            (uint64_t)context->operations[operation].id + UINT64_C(1),\n"
        "            &operation_forbidden, &operation_priority,\n"
        "            &oracle_diagnostic);\n"
        "        if (oracle_status != ARCHON_ORACLE_STATUS_OK) {\n"
        "            (void)AP_fail(\n"
        "                context, \"invalid rewrite operation annotation: \"\n"
        "                \"%s%s%s\",\n"
        "                archon_oracle_status_string(oracle_status),\n"
        "                oracle_diagnostic.message[0] != '\\0'\n"
        "                    ? \": \" : \"\",\n"
        "                oracle_diagnostic.message);\n"
        "            success = 0;\n"
        "            goto rewrite_done;\n"
        "        }\n"
        "        forbidden[operation] = operation_forbidden != 0;\n"
        "        priorities[operation] = operation_priority;\n"
        if oracle_enabled
        else ""
    )
    oracle_rewrite_dispose = (
        "    archon_rewrite_oracle_config_dispose(&oracle_config);\n"
        if oracle_enabled
        else ""
    )
    adapter_declarations, adapter_body = _adapter(representation).split(
        "@ADAPTER_BODY@", 1
    )
    source = (
        _SOURCE.replace("@REPRESENTATION@", _c_string(representation))
        .replace("@WORD_WIDTH@", str(_word_width(db)))
        .replace("@STACK_TARGET@", str(int(_is_stack_target(db))))
        .replace("@ADAPTER_DECLARATIONS@", adapter_declarations)
        .replace("@ADAPTER_BODY@", adapter_body)
        .replace("@SEA_TARGET_ROWS@", _sea_targets(db))
        .replace("@ORACLE_INCLUDE@", oracle_include)
        .replace("@ORACLE_SUPPORT@", oracle_support)
        .replace("@ORACLE_PROCESS_REWRITE@", oracle_rewrite)
        .replace("@ORACLE_PROCESS_MIR@", oracle_mir)
        .replace("@ORACLE_PROCESS_SCHEDULED@", oracle_scheduled)
        .replace("@ORACLE_REWRITE_PREPARE@", oracle_rewrite_prepare)
        .replace(
            "@ORACLE_REWRITE_ACTIVE_PASS@", oracle_rewrite_active_pass
        )
        .replace("@ORACLE_CONTEXT_FIELD@", oracle_context_field)
        .replace("@ORACLE_CONTEXT_INIT@", oracle_context_init)
        .replace("@ORACLE_EMITTER_LOCAL@", oracle_emitter_local)
        .replace("@ORACLE_EMITTER_ARGUMENT@", oracle_emitter_argument)
        .replace(
            "@ORACLE_ANNOTATION_CONTEXT_INIT@",
            oracle_annotation_context_init,
        )
        .replace(
            "@ORACLE_ANNOTATION_CONTEXT_DISPOSE@",
            oracle_annotation_context_dispose,
        )
        .replace("@ORACLE_SELECT_PREPARE@", oracle_select_prepare)
        .replace("@ORACLE_SELECT_CALL@", oracle_select_call)
        .replace("@ORACLE_ISCHED_PREPARE@", oracle_isched_prepare)
        .replace("@ORACLE_ISCHED_CALL@", oracle_isched_call)
        .replace("@ORACLE_PROCESS_REGMAP@", oracle_regmap)
        .replace("@ORACLE_REWRITE_LOCALS@", oracle_rewrite_locals)
        .replace("@ORACLE_REWRITE_LOAD@", oracle_rewrite_load)
        .replace("@ORACLE_REWRITE_OPERATION@", oracle_rewrite_operation)
        .replace("@ORACLE_REWRITE_DISPOSE@", oracle_rewrite_dispose)
    )
    return {
        "MacroABI.h": macro_abi_header(),
        "Pipeline.h": (
            _HEADER.replace("@ORACLE_INCLUDE@", oracle_include)
            .replace("@ORACLE_OPTION_FIELD@", oracle_option_field)
        ),
        "Pipeline.c": source,
    }


_HEADER = r'''/* Generated by Archon: linked representation pipeline. */
#ifndef ARCHON_GENERATED_PIPELINE_H
#define ARCHON_GENERATED_PIPELINE_H

#include "Emitter.h"
@ORACLE_INCLUDE@

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ArchonPipelineStage {
    ARCHON_PIPELINE_DECODE = 0,
    ARCHON_PIPELINE_REWRITE,
    ARCHON_PIPELINE_SCHEDULE,
    ARCHON_PIPELINE_ISEL,
    ARCHON_PIPELINE_ISCHED,
    ARCHON_PIPELINE_REGALLOC,
    ARCHON_PIPELINE_EMIT
} ArchonPipelineStage;

typedef enum ArchonPipelineRewrite {
    ARCHON_PIPELINE_REWRITE_CONVENTIONAL = 0,
    ARCHON_PIPELINE_REWRITE_EGRAPH
} ArchonPipelineRewrite;

#define ARCHON_PIPELINE_MAX_DIAGNOSTIC_BYTES 255u

typedef enum ArchonPipelineStatus {
    ARCHON_PIPELINE_STATUS_OK = 0,
    ARCHON_PIPELINE_STATUS_INVALID_ARGUMENT,
    ARCHON_PIPELINE_STATUS_STAGE_FAILED
} ArchonPipelineStatus;

typedef struct ArchonPipelineOptions {
    ArchonPipelineStage stop_after;
    ArchonPipelineRewrite rewrite;
    const Emitter_Options *emitter;
    FILE *diagnostics;
@ORACLE_OPTION_FIELD@
} ArchonPipelineOptions;

typedef struct ArchonPipelineResult {
    ArchonPipelineStatus status;
    ArchonPipelineStage stage;
    unsigned stage_valid;
    Emitter_Status emitter_status;
    unsigned emitter_artifact_line;
    char diagnostic[ARCHON_PIPELINE_MAX_DIAGNOSTIC_BYTES + 1u];
} ArchonPipelineResult;

void archon_pipeline_options_init(ArchonPipelineOptions *options);
void archon_pipeline_result_init(ArchonPipelineResult *result);
ArchonPipelineStatus archon_pipeline_run_with_result(
    const ArchonPipelineOptions *options,
    ArchonPipelineResult *result);
int archon_pipeline_run(const ArchonPipelineOptions *options);
int archon_pipeline_emit_program(
    FILE *output, char *diagnostic, size_t diagnostic_capacity);
int archon_pipeline_emit_mir(
    FILE *output, char *diagnostic, size_t diagnostic_capacity);
const char *archon_pipeline_representation(void);

#ifdef __cplusplus
}
#endif
#endif
'''


_ORACLE_SUPPORT = r'''
static int AP_oracle_active(const AP_Context *context) {
    return context != NULL && context->oracle_options != NULL &&
           context->oracle_options->socket_path != NULL &&
           context->oracle_options->socket_path[0] != '\0';
}

static int AP_oracle_annotation_path(
        AP_Context *context, const char *artifact_path,
        char *output, size_t output_capacity) {
    const char *configured = NULL;
    ArchonOracleStatus status;
    if (context != NULL && context->oracle_options != NULL)
        configured = context->oracle_options->annotation_path;
    if (configured != NULL && configured[0] != '\0') {
        size_t length = strlen(configured);
        if (length > ARCHON_ORACLE_MAX_PATH_BYTES ||
                length + 1u > output_capacity)
            return AP_fail(
                context, "Oracle annotation path exceeds its limit");
        memcpy(output, configured, length + 1u);
        return 1;
    }
    status = archon_oracle_default_annotation_path(
        artifact_path, output, output_capacity);
    if (status == ARCHON_ORACLE_STATUS_OK)
        return 1;
    return AP_fail(
        context, "cannot derive Oracle annotation path for %s: %s",
        artifact_path, archon_oracle_status_string(status));
}

static int AP_oracle_load_annotations(
        AP_Context *context, const char *artifact_path,
        ArchonOracleStage expected_stage,
        ArchonOracleAnnotations *annotations, int *loaded) {
    char sidecar[ARCHON_ORACLE_MAX_PATH_BYTES + 1u];
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    if (annotations == NULL || loaded == NULL)
        return AP_fail(context, "invalid Oracle annotation destination");
    *loaded = 0;
    archon_oracle_annotations_dispose(annotations);
    archon_oracle_annotations_init(annotations);
    if (!AP_oracle_annotation_path(
            context, artifact_path, sidecar, sizeof sidecar))
        return 0;
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_annotations_load(
        sidecar, annotations, &diagnostic);
    if (status == ARCHON_ORACLE_STATUS_NOT_FOUND)
        return 1;
    if (status != ARCHON_ORACLE_STATUS_OK)
        return AP_fail(
            context, "cannot load Oracle annotations for %s: %s%s%s",
            artifact_path, archon_oracle_status_string(status),
            diagnostic.message[0] != '\0' ? ": " : "",
            diagnostic.message);
    if (annotations->stage != expected_stage) {
        ArchonOracleStage actual_stage = annotations->stage;
        archon_oracle_annotations_dispose(annotations);
        archon_oracle_annotations_init(annotations);
        return AP_fail(
            context, "Oracle annotations for %s have stage %s, expected %s",
            artifact_path,
            archon_oracle_stage_string(actual_stage),
            archon_oracle_stage_string(expected_stage));
    }
    *loaded = 1;
    return 1;
}

static int AP_oracle_snapshot(
        AP_Context *context, const char *path,
        unsigned char **bytes, size_t *size) {
    FILE *input;
    long length;
    size_t maximum = ARCHON_ORACLE_MAX_ARTIFACT_BYTES;
    unsigned char *data;
    if (bytes == NULL || size == NULL)
        return AP_fail(context, "invalid Oracle snapshot output");
    *bytes = NULL;
    *size = 0u;
    if (context->oracle_options != NULL &&
            context->oracle_options->max_artifact_bytes != 0u)
        maximum = context->oracle_options->max_artifact_bytes;
    input = fopen(path, "rb");
    if (input == NULL)
        return AP_fail(context, "cannot snapshot %s for Oracle", path);
    if (fseek(input, 0L, SEEK_END) != 0) {
        (void)fclose(input);
        return AP_fail(context, "cannot size %s for Oracle", path);
    }
    length = ftell(input);
    if (length < 0 || (uint64_t)length > (uint64_t)maximum) {
        (void)fclose(input);
        return AP_fail(
            context, "%s exceeds the Oracle artifact limit", path);
    }
    if (fseek(input, 0L, SEEK_SET) != 0) {
        (void)fclose(input);
        return AP_fail(context, "cannot rewind %s for Oracle", path);
    }
    data = (unsigned char *)malloc(
        length != 0L ? (size_t)length : 1u);
    if (data == NULL) {
        (void)fclose(input);
        return AP_fail(context, "out of memory snapshotting %s", path);
    }
    if (length != 0L &&
            fread(data, 1u, (size_t)length, input) != (size_t)length) {
        free(data);
        (void)fclose(input);
        return AP_fail(context, "cannot read %s for Oracle", path);
    }
    if (fclose(input) != 0) {
        free(data);
        return AP_fail(context, "cannot close Oracle snapshot for %s", path);
    }
    *bytes = data;
    *size = (size_t)length;
    return 1;
}

static int AP_oracle_process(
        AP_Context *context, const char *path, ArchonOracleStage stage) {
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    unsigned char *before = NULL;
    unsigned char *after = NULL;
    size_t before_size = 0u;
    size_t after_size = 0u;
    int active = AP_oracle_active(context);
    if (active &&
            !AP_oracle_snapshot(context, path, &before, &before_size))
        return 0;
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_process_file(
        path, stage, context->oracle_options, &diagnostic);
    if (status == ARCHON_ORACLE_STATUS_OK && active) {
        char sidecar[ARCHON_ORACLE_MAX_PATH_BYTES + 1u];
        if (!AP_oracle_snapshot(context, path, &after, &after_size)) {
            free(before);
            return 0;
        }
        if (before_size != after_size ||
                (before_size != 0u &&
                 memcmp(before, after, before_size) != 0)) {
            if (AP_oracle_annotation_path(
                    context, path, sidecar, sizeof sidecar))
                (void)remove(sidecar);
            free(after);
            free(before);
            return AP_fail(
                context,
                "Oracle artifact transformations are unsupported by "
                "the linked in-memory pipeline: %s", path);
        }
    }
    free(after);
    free(before);
    if (status == ARCHON_ORACLE_STATUS_OK ||
            status == ARCHON_ORACLE_STATUS_INACTIVE)
        return 1;
    return AP_fail(
        context, "Oracle rejected %s: %s%s%s",
        path, archon_oracle_status_string(status),
        diagnostic.message[0] != '\0' ? ": " : "",
        diagnostic.message);
}

static int AP_oracle_process_default_sidecar(
        AP_Context *context, const char *path, ArchonOracleStage stage) {
    const ArchonOracleOptions *saved = context->oracle_options;
    ArchonOracleOptions local;
    int success;
    if (saved != NULL) {
        local = *saved;
        /*
         * Regalloc's public in-memory entry point consumes the canonical
         * artifact-derived sidecar path.  Preserve every other client option.
         */
        local.annotation_path = NULL;
        context->oracle_options = &local;
    }
    success = AP_oracle_process(context, path, stage);
    context->oracle_options = saved;
    return success;
}
'''


_SOURCE = r'''/* Generated by Archon: strict ISO C99 linked pipeline. */
#include "Pipeline.h"

#include "IR.h"
#include "ISched.h"
#include "ISel.h"
#include "MacroABI.h"
@ORACLE_INCLUDE@
#include "Regalloc.h"
#include "Rewrite.h"

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    AP_WORD_WIDTH = @WORD_WIDTH@,
    AP_STACK_TARGET = @STACK_TARGET@
};

static const char AP_representation[] = @REPRESENTATION@;

typedef enum AP_InputKind {
    AP_INPUT_VALUE = 1,
    AP_INPUT_IMMEDIATE,
    AP_INPUT_SYMBOL
} AP_InputKind;

typedef struct AP_Value {
    uint32_t id;
    unsigned width;
    unsigned is_signed;
    unsigned is_constant;
    unsigned has_definition;
    uint64_t bits;
} AP_Value;

typedef struct AP_Input {
    AP_InputKind kind;
    uint32_t value;
    int64_t immediate;
    unsigned width;
    unsigned is_signed;
    const char *symbol;
} AP_Input;

typedef struct AP_Block {
    uint32_t id;
    const char *name;
    size_t first_operation;
    size_t operation_count;
} AP_Block;

typedef struct AP_Operation {
    uint32_t id;
    uint32_t block;
    const char *primitive;
    const char *target;
    const char *unsupported;
    uint32_t result;
    unsigned has_result;
    unsigned result_width;
    AP_Input *inputs;
    size_t input_count;
    unsigned barrier;
} AP_Operation;

typedef struct AP_Selected {
    size_t operation;
    size_t instruction;
} AP_Selected;

typedef struct AP_Context AP_Context;

@ADAPTER_DECLARATIONS@

struct AP_Context {
    AP_AdapterProgram program;
    AP_Value *values;
    size_t value_count;
    size_t value_capacity;
    AP_Block *blocks;
    size_t block_count;
    size_t block_capacity;
    AP_Operation *operations;
    size_t operation_count;
    size_t operation_capacity;
    AP_Selected *selected;
    size_t selected_count;
    ISched_Operation *schedule_operations;
    ISched_Dependency *dependencies;
    size_t dependency_count;
    ISched_Resource resource;
    ISched_Problem problem;
    ISched_Result schedule;
    struct RA_allocator allocator;
    unsigned allocator_initialized;
    unsigned decoded;
    uint64_t program_stream_id;
    uint64_t mir_stream_id;
    uint64_t instr_map_stream_id;
    uint64_t scheduled_stream_id;
    uint64_t reg_map_stream_id;
@ORACLE_CONTEXT_FIELD@
    char diagnostic[256];
};

const char *archon_pipeline_representation(void) {
    return AP_representation;
}

static int AP_fail(AP_Context *context, const char *format, ...) {
    va_list arguments;
    if (context != NULL && context->diagnostic[0] == '\0') {
        va_start(arguments, format);
        (void)vsnprintf(
            context->diagnostic, sizeof context->diagnostic,
            format, arguments);
        va_end(arguments);
        context->diagnostic[sizeof context->diagnostic - 1u] = '\0';
    }
    return 0;
}

@ORACLE_SUPPORT@

static int AP_reserve(void **data, size_t *capacity, size_t count,
                      size_t element_size) {
    size_t next;
    void *grown;
    if (count <= *capacity)
        return 1;
    next = *capacity != 0u ? *capacity : 16u;
    while (next < count) {
        if (next > SIZE_MAX / 2u)
            return 0;
        next *= 2u;
    }
    if (element_size != 0u && next > SIZE_MAX / element_size)
        return 0;
    grown = realloc(*data, next * element_size);
    if (grown == NULL)
        return 0;
    *data = grown;
    *capacity = next;
    return 1;
}

static AP_Value *AP_find_value(AP_Context *context, uint32_t id) {
    size_t index;
    for (index = 0u; index < context->value_count; ++index)
        if (context->values[index].id == id)
            return &context->values[index];
    return NULL;
}

static int AP_add_value(AP_Context *context, uint32_t id, unsigned width,
                        unsigned is_signed, unsigned is_constant,
                        uint64_t bits) {
    AP_Value *value;
    if (id == UINT32_MAX || AP_find_value(context, id) != NULL)
        return AP_fail(context, "duplicate or invalid value %" PRIu32, id);
    if (width == 0u)
        width = AP_WORD_WIDTH;
    if (!AP_reserve(
            (void **)&context->values, &context->value_capacity,
            context->value_count + 1u, sizeof *context->values))
        return AP_fail(context, "out of memory while adding a value");
    value = &context->values[context->value_count++];
    value->id = id;
    value->width = width;
    value->is_signed = is_signed != 0u;
    value->is_constant = is_constant != 0u;
    value->has_definition = 0u;
    value->bits = bits;
    return 1;
}

static int64_t AP_signed_bits(uint64_t bits, unsigned width,
                              unsigned is_signed) {
    uint64_t mask, sign;
    if (!is_signed || width == 0u || width >= 64u)
        return (int64_t)bits;
    mask = (UINT64_C(1) << width) - UINT64_C(1);
    sign = UINT64_C(1) << (width - 1u);
    bits &= mask;
    if ((bits & sign) != 0u)
        return -(int64_t)(((~bits) & mask) + UINT64_C(1));
    return (int64_t)bits;
}

static int AP_value_input(AP_Context *context, uint32_t id, AP_Input *input) {
    AP_Value *value = AP_find_value(context, id);
    if (value == NULL)
        return AP_fail(context, "use of unknown value %" PRIu32, id);
    memset(input, 0, sizeof *input);
    input->width = value->width;
    input->is_signed = value->is_signed;
    if (value->is_constant) {
        input->kind = AP_INPUT_IMMEDIATE;
        input->immediate = AP_signed_bits(
            value->bits, value->width, value->is_signed);
    } else {
        input->kind = AP_INPUT_VALUE;
        input->value = id;
    }
    return 1;
}

static int AP_add_block(AP_Context *context, uint32_t id, const char *name) {
    AP_Block *block;
    size_t index;
    for (index = 0u; index < context->block_count; ++index)
        if (context->blocks[index].id == id)
            return AP_fail(context, "duplicate block %" PRIu32, id);
    if (!AP_reserve(
            (void **)&context->blocks, &context->block_capacity,
            context->block_count + 1u, sizeof *context->blocks))
        return AP_fail(context, "out of memory while adding a block");
    block = &context->blocks[context->block_count++];
    block->id = id;
    block->name = name != NULL ? name : "";
    block->first_operation = context->operation_count;
    block->operation_count = 0u;
    return 1;
}

static AP_Block *AP_find_block(AP_Context *context, uint32_t id) {
    size_t index;
    for (index = 0u; index < context->block_count; ++index)
        if (context->blocks[index].id == id)
            return &context->blocks[index];
    return NULL;
}

static uint64_t AP_mir_block_id(
        const AP_Context *context, uint32_t source_id) {
    size_t index;
    for (index = 0u; index < context->block_count; ++index)
        if (context->blocks[index].id == source_id)
            return (uint64_t)index + UINT64_C(1);
    return UINT64_C(0);
}

static size_t AP_operation_ordinal(
        const AP_Context *context, size_t operation_index) {
    size_t index, ordinal = 0u;
    uint32_t block = context->operations[operation_index].block;
    for (index = 0u; index < operation_index; ++index)
        if (context->operations[index].block == block)
            ++ordinal;
    return ordinal;
}

static int AP_add_operation(
        AP_Context *context, uint32_t block_id,
        const char *primitive, const char *target, const char *unsupported,
        unsigned has_result, uint32_t result, unsigned result_width,
        const AP_Input *inputs, size_t input_count, unsigned barrier) {
    AP_Operation *operation;
    AP_Block *block = AP_find_block(context, block_id);
    if (block == NULL)
        return AP_fail(context, "operation refers to unknown block");
    if (input_count != 0u && inputs == NULL)
        return AP_fail(context, "operation has null inputs");
    if (!AP_reserve(
            (void **)&context->operations, &context->operation_capacity,
            context->operation_count + 1u, sizeof *context->operations))
        return AP_fail(context, "out of memory while adding an operation");
    operation = &context->operations[context->operation_count];
    memset(operation, 0, sizeof *operation);
    operation->id = (uint32_t)context->operation_count;
    operation->block = block_id;
    operation->primitive = primitive;
    operation->target = target;
    operation->unsupported = unsupported;
    operation->has_result = has_result != 0u;
    operation->result = result;
    operation->result_width = result_width != 0u ? result_width : AP_WORD_WIDTH;
    operation->input_count = input_count;
    operation->barrier = barrier != 0u;
    if (input_count != 0u) {
        if (input_count > SIZE_MAX / sizeof *operation->inputs)
            return AP_fail(context, "operation input count is too large");
        operation->inputs = (AP_Input *)malloc(
            input_count * sizeof *operation->inputs);
        if (operation->inputs == NULL)
            return AP_fail(context, "out of memory while copying inputs");
        memcpy(operation->inputs, inputs, input_count * sizeof *inputs);
    }
    ++context->operation_count;
    ++block->operation_count;
    if (has_result) {
        if (!AP_add_value(
                context, result, operation->result_width, 0u, 0u, 0u))
            return 0;
        context->values[context->value_count - 1u].has_definition = 1u;
    }
    return 1;
}

@ADAPTER_BODY@

static uint64_t AP_hash_bytes(uint64_t value, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    size_t index;
    for (index = 0u; index < size; ++index) {
        value ^= bytes[index];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

static uint64_t AP_hash_u64(uint64_t value, uint64_t number) {
    unsigned char bytes[8];
    unsigned shift;
    for (shift = 0u; shift < 64u; shift += 8u)
        bytes[shift / 8u] = (unsigned char)(number >> shift);
    return AP_hash_bytes(value, bytes, sizeof bytes);
}

static uint64_t AP_hash_text(uint64_t value, const char *text) {
    size_t size;
    if (text == NULL)
        return AP_hash_u64(value, UINT64_MAX);
    size = strlen(text);
    value = AP_hash_u64(value, (uint64_t)size);
    return AP_hash_bytes(value, text, size);
}

static uint64_t AP_derive_id(uint64_t parent, const char *tag) {
    uint64_t value = AP_hash_u64(
        UINT64_C(1469598103934665603), parent);
    value = AP_hash_text(value, tag);
    return value != 0u ? value : UINT64_C(1);
}

static void AP_assign_stream_ids(AP_Context *context) {
    uint64_t value = AP_hash_text(
        UINT64_C(1469598103934665603), AP_representation);
    size_t index, input;
    value = AP_hash_u64(value, (uint64_t)context->block_count);
    for (index = 0u; index < context->block_count; ++index) {
        value = AP_hash_u64(value, context->blocks[index].id);
        value = AP_hash_text(value, context->blocks[index].name);
    }
    value = AP_hash_u64(value, (uint64_t)context->value_count);
    for (index = 0u; index < context->value_count; ++index) {
        const AP_Value *row = &context->values[index];
        value = AP_hash_u64(value, row->id);
        value = AP_hash_u64(value, row->width);
        value = AP_hash_u64(value, row->is_signed);
        value = AP_hash_u64(value, row->is_constant);
        value = AP_hash_u64(value, row->bits);
    }
    value = AP_hash_u64(value, (uint64_t)context->operation_count);
    for (index = 0u; index < context->operation_count; ++index) {
        const AP_Operation *operation = &context->operations[index];
        value = AP_hash_u64(value, operation->id);
        value = AP_hash_u64(value, operation->block);
        value = AP_hash_text(value, operation->primitive);
        value = AP_hash_text(value, operation->target);
        value = AP_hash_text(value, operation->unsupported);
        value = AP_hash_u64(value, operation->result);
        value = AP_hash_u64(value, operation->has_result);
        value = AP_hash_u64(value, operation->result_width);
        value = AP_hash_u64(value, (uint64_t)operation->input_count);
        value = AP_hash_u64(value, operation->barrier);
        for (input = 0u; input < operation->input_count; ++input) {
            const AP_Input *row = &operation->inputs[input];
            value = AP_hash_u64(value, row->kind);
            value = AP_hash_u64(value, row->value);
            value = AP_hash_u64(value, (uint64_t)row->immediate);
            value = AP_hash_u64(value, row->width);
            value = AP_hash_u64(value, row->is_signed);
            value = AP_hash_text(value, row->symbol);
        }
    }
    context->program_stream_id = value != 0u ? value : UINT64_C(1);
    context->mir_stream_id = AP_derive_id(
        context->program_stream_id, "MIR");
    context->instr_map_stream_id = AP_derive_id(
        context->mir_stream_id, "InstrMap");
    context->scheduled_stream_id = AP_derive_id(
        context->instr_map_stream_id, "Scheduled");
    context->reg_map_stream_id = AP_derive_id(
        context->scheduled_stream_id, "RegMap");
}

static int AP_write_string(FILE *output, const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    if (text == NULL || fputc('"', output) == EOF)
        return 0;
    while (*cursor != 0u) {
        unsigned char byte = *cursor++;
        if (byte == '"' || byte == '\\') {
            if (fputc('\\', output) == EOF ||
                    fputc((int)byte, output) == EOF)
                return 0;
        } else if (byte >= 0x20u && byte <= 0x7eu && byte != '?') {
            if (fputc((int)byte, output) == EOF)
                return 0;
        } else if (fprintf(output, "\\%03o", (unsigned)byte) < 0) {
            return 0;
        }
    }
    return fputc('"', output) != EOF;
}

static int AP_write_common_header(
        FILE *output, const char *artifact, const char *schema) {
    return fprintf(
        output,
        "/* Generated by Archon linked pipeline; canonical C99 X-macro "
        "stream. */\n#include \"MacroABI.h\"\n\n"
        "#if ARCHON_MACRO_ABI_VERSION != 0x00010000UL\n"
        "# error \"%s requires Archon Macro ABI 1.0.0\"\n"
        "#endif\n"
        "#if %s != 1UL\n"
        "# error \"%s schema version is incompatible with MacroABI.h\"\n"
        "#endif\n\n",
        artifact, schema, artifact) >= 0;
}

static const char *AP_opcode(const AP_Operation *operation);
static uint64_t AP_width_bits(uint64_t bits, unsigned width);

static int AP_program_prologue(FILE *output) {
    return AP_write_common_header(
               output, "Program.i", "ARCHON_PROGRAM_SCHEMA_VERSION") &&
        fputs(
            "#ifdef ARCHON_MACRO_ABI_ACTIVE_PROGRAM\n"
            "# error \"recursive inclusion of Program.i\"\n"
            "#endif\n"
            "#define ARCHON_MACRO_ABI_ACTIVE_PROGRAM 1\n\n"
            "#ifndef ARCHON_PROGRAM_BEGIN\n"
            "# define ARCHON_PROGRAM_BEGIN(A0,A1,A2,A3,A4)\n"
            "# define AP_LOCAL_PROGRAM_BEGIN 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_BLOCK\n"
            "# define ARCHON_PROGRAM_BLOCK(A0,A1,A2,A3)\n"
            "# define AP_LOCAL_PROGRAM_BLOCK 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_OP\n"
            "# define ARCHON_PROGRAM_OP(A0,A1,A2,A3,A4,A5,A6,A7,A8,A9,A10)\n"
            "# define AP_LOCAL_PROGRAM_OP 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_OPERAND\n"
            "# define ARCHON_PROGRAM_OPERAND(A0,A1,A2,A3)\n"
            "# define AP_LOCAL_PROGRAM_OPERAND 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_LITERAL\n"
            "# define ARCHON_PROGRAM_LITERAL(A0,A1,A2,A3,A4,A5)\n"
            "# define AP_LOCAL_PROGRAM_LITERAL 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_SYMBOL\n"
            "# define ARCHON_PROGRAM_SYMBOL(A0,A1,A2,A3,A4,A5)\n"
            "# define AP_LOCAL_PROGRAM_SYMBOL 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_TEXT\n"
            "# define ARCHON_PROGRAM_TEXT(A0,A1,A2,A3,A4)\n"
            "# define AP_LOCAL_PROGRAM_TEXT 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_ATTRIBUTE\n"
            "# define ARCHON_PROGRAM_ATTRIBUTE(A0,A1,A2,A3,A4,A5,A6,A7)\n"
            "# define AP_LOCAL_PROGRAM_ATTRIBUTE 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_SUCCESSOR\n"
            "# define ARCHON_PROGRAM_SUCCESSOR(A0,A1,A2,A3)\n"
            "# define AP_LOCAL_PROGRAM_SUCCESSOR 1\n#endif\n"
            "#ifndef ARCHON_PROGRAM_END\n"
            "# define ARCHON_PROGRAM_END(A0)\n"
            "# define AP_LOCAL_PROGRAM_END 1\n#endif\n\n",
            output) != EOF;
}

static int AP_program_epilogue(FILE *output) {
    return fputs(
        "\n#ifdef AP_LOCAL_PROGRAM_END\n# undef ARCHON_PROGRAM_END\n"
        "# undef AP_LOCAL_PROGRAM_END\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_SUCCESSOR\n"
        "# undef ARCHON_PROGRAM_SUCCESSOR\n"
        "# undef AP_LOCAL_PROGRAM_SUCCESSOR\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_ATTRIBUTE\n"
        "# undef ARCHON_PROGRAM_ATTRIBUTE\n"
        "# undef AP_LOCAL_PROGRAM_ATTRIBUTE\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_TEXT\n# undef ARCHON_PROGRAM_TEXT\n"
        "# undef AP_LOCAL_PROGRAM_TEXT\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_SYMBOL\n# undef ARCHON_PROGRAM_SYMBOL\n"
        "# undef AP_LOCAL_PROGRAM_SYMBOL\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_LITERAL\n# undef ARCHON_PROGRAM_LITERAL\n"
        "# undef AP_LOCAL_PROGRAM_LITERAL\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_OPERAND\n# undef ARCHON_PROGRAM_OPERAND\n"
        "# undef AP_LOCAL_PROGRAM_OPERAND\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_OP\n# undef ARCHON_PROGRAM_OP\n"
        "# undef AP_LOCAL_PROGRAM_OP\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_BLOCK\n# undef ARCHON_PROGRAM_BLOCK\n"
        "# undef AP_LOCAL_PROGRAM_BLOCK\n#endif\n"
        "#ifdef AP_LOCAL_PROGRAM_BEGIN\n# undef ARCHON_PROGRAM_BEGIN\n"
        "# undef AP_LOCAL_PROGRAM_BEGIN\n#endif\n"
        "#undef ARCHON_MACRO_ABI_ACTIVE_PROGRAM\n",
        output) != EOF;
}

static int AP_value_is_produced(
        const AP_Context *context, uint32_t value) {
    size_t operation;
    for (operation = 0u; operation < context->operation_count; ++operation)
        if (context->operations[operation].has_result &&
                context->operations[operation].result == value)
            return 1;
    return 0;
}

static size_t AP_parameter_count(const AP_Context *context) {
    size_t count = 0u, value;
    for (value = 0u; value < context->value_count; ++value)
        if (!context->values[value].is_constant &&
                !AP_value_is_produced(context, context->values[value].id))
            ++count;
    return count;
}

static uint64_t AP_program_parameter_operation_id(uint32_t value) {
    return UINT64_C(0x8000000000000000) |
           ((uint64_t)value + UINT64_C(1));
}

static int AP_emit_program(AP_Context *context, FILE *output) {
    size_t block, operation, value;
    size_t parameters = AP_parameter_count(context);
    size_t parameter_ordinal = 0u;
    if (output == NULL)
        return AP_fail(context, "null Program output stream");
    if (context->block_count == 0u)
        return AP_fail(context, "neutral program has no blocks");
    if (context->block_count != 1u)
        return AP_fail(
            context,
            "canonical Program CFG lowering is unavailable for %zu blocks",
            context->block_count);
    if (!AP_program_prologue(output) ||
        fprintf(
            output,
            "ARCHON_PROGRAM_BEGIN(ARCHON_MACRO_ABI_VERSION, "
            "ARCHON_PROGRAM_SCHEMA_VERSION, UINT64_C(0x%016" PRIX64 "), "
            "UINT32_C(%zu), UINT32_C(%zu))\n",
            context->program_stream_id, context->block_count,
            context->operation_count + parameters) < 0)
        goto failed;
    for (block = 0u; block < context->block_count; ++block) {
        if (fprintf(
                output,
                "ARCHON_PROGRAM_BLOCK(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%zu), ",
                (uint64_t)block + UINT64_C(1), block) < 0 ||
            !AP_write_string(output, context->blocks[block].name) ||
            fputs(", UINT32_C(0))\n", output) == EOF)
            goto failed;
    }
    for (value = 0u; value < context->value_count; ++value) {
        const AP_Value *row = &context->values[value];
        if (row->is_constant ||
                AP_value_is_produced(context, row->id))
            continue;
        if (fprintf(
                output,
                "ARCHON_PROGRAM_OP(UINT64_C(0x%016" PRIX64 "), "
                "UINT64_C(0x0000000000000001), UINT32_C(%zu), "
                "ARCHON_ID_706172616D65746572, "
                "UINT64_C(0x%016" PRIX64 "), %s, UINT32_C(%u), "
                "UINT32_C(0), UINT32_C(0), UINT32_C(0), UINT32_C(8))\n",
                AP_program_parameter_operation_id(row->id),
                parameter_ordinal++,
                (uint64_t)row->id + UINT64_C(1),
                row->is_signed ? "ARCHON_TYPE_SINT" : "ARCHON_TYPE_BITS",
                row->width) < 0)
            goto failed;
    }
    for (block = 0u; block < context->block_count; ++block) {
        for (operation = 0u;
             operation < context->operation_count; ++operation) {
            const AP_Operation *row = &context->operations[operation];
            size_t ordinal;
            unsigned flags;
            if (row->block != context->blocks[block].id)
                continue;
            ordinal = AP_operation_ordinal(context, operation);
            if (block == 0u)
                ordinal += parameters;
            flags = (row->barrier ? 1u : 0u) |
                    (row->target != NULL ? 2u : 0u) |
                    (row->unsupported != NULL ? 4u : 0u);
            if (fprintf(
                    output,
                    "ARCHON_PROGRAM_OP(UINT64_C(0x%016" PRIX64 "), "
                    "UINT64_C(0x%016" PRIX64 "), UINT32_C(%zu), %s, "
                    "UINT64_C(0x%016" PRIX64 "), %s, "
                    "UINT32_C(%u), UINT32_C(%zu), UINT32_C(0), "
                    "UINT32_C(0), UINT32_C(%u))\n",
                    (uint64_t)row->id + UINT64_C(1),
                    AP_mir_block_id(context, row->block), ordinal,
                    AP_opcode(row),
                    row->has_result
                        ? (uint64_t)row->result + UINT64_C(1)
                        : UINT64_C(0),
                    row->has_result
                        ? "ARCHON_TYPE_BITS" : "ARCHON_TYPE_VOID",
                    row->has_result ? row->result_width : 0u,
                    row->input_count, flags) < 0)
                goto failed;
        }
    }
    for (operation = 0u; operation < context->operation_count; ++operation) {
        const AP_Operation *row = &context->operations[operation];
        size_t input;
        for (input = 0u; input < row->input_count; ++input) {
            const AP_Input *operand = &row->inputs[input];
            if (operand->kind == AP_INPUT_VALUE &&
                fprintf(
                    output,
                    "ARCHON_PROGRAM_OPERAND(UINT64_C(0x%016" PRIX64 "), "
                    "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), "
                    "UINT32_C(0))\n",
                    (uint64_t)row->id + UINT64_C(1), input,
                    (uint64_t)operand->value + UINT64_C(1)) < 0)
                goto failed;
        }
    }
    for (operation = 0u; operation < context->operation_count; ++operation) {
        const AP_Operation *row = &context->operations[operation];
        size_t input;
        for (input = 0u; input < row->input_count; ++input) {
            const AP_Input *operand = &row->inputs[input];
            if (operand->kind == AP_INPUT_IMMEDIATE) {
                uint64_t bits = AP_width_bits(
                    (uint64_t)operand->immediate,
                    operand->width != 0u
                        ? operand->width : AP_WORD_WIDTH);
                if (fprintf(
                        output,
                        "ARCHON_PROGRAM_LITERAL("
                        "UINT64_C(0x%016" PRIX64 "), UINT32_C(%zu), %s, "
                        "UINT32_C(%u), \"%" PRIX64 "\", UINT32_C(0))\n",
                        (uint64_t)row->id + UINT64_C(1), input,
                        operand->is_signed
                            ? "ARCHON_TYPE_SINT" : "ARCHON_TYPE_BITS",
                        operand->width != 0u
                            ? operand->width : AP_WORD_WIDTH,
                        bits) < 0)
                    goto failed;
            }
        }
    }
    for (operation = 0u; operation < context->operation_count; ++operation) {
        const AP_Operation *row = &context->operations[operation];
        size_t input;
        for (input = 0u; input < row->input_count; ++input) {
            const AP_Input *operand = &row->inputs[input];
            if (operand->kind == AP_INPUT_SYMBOL &&
                (fprintf(
                     output,
                     "ARCHON_PROGRAM_SYMBOL("
                     "UINT64_C(0x%016" PRIX64 "), UINT32_C(%zu), "
                     "ARCHON_SYMBOL_OPAQUE, ",
                     (uint64_t)row->id + UINT64_C(1), input) < 0 ||
                 !AP_write_string(output, operand->symbol) ||
                 fputs(", INT64_C(0), UINT32_C(0))\n", output) == EOF))
                goto failed;
        }
    }
    if (fprintf(
            output,
            "ARCHON_PROGRAM_END(UINT64_C(0x%016" PRIX64 "))\n",
            context->program_stream_id) < 0 ||
        !AP_program_epilogue(output))
        goto failed;
    return 1;

failed:
    if (context->diagnostic[0] == '\0')
        (void)AP_fail(context, "cannot write canonical Program stream");
    return 0;
}

static int AP_mir_prologue(FILE *output) {
    return AP_write_common_header(
               output, "MIR.i", "ARCHON_MIR_SCHEMA_VERSION") &&
        fputs(
            "#ifdef ARCHON_MACRO_ABI_ACTIVE_MIR\n"
            "# error \"recursive inclusion of MIR.i\"\n"
            "#endif\n"
            "#define ARCHON_MACRO_ABI_ACTIVE_MIR 1\n\n"
            "#ifndef ARCHON_MIR_BEGIN\n"
            "# define ARCHON_MIR_BEGIN(A0,A1,A2,A3,A4,A5,A6)\n"
            "# define AP_LOCAL_MIR_BEGIN 1\n#endif\n"
            "#ifndef ARCHON_MIR_BLOCK\n"
            "# define ARCHON_MIR_BLOCK(A0,A1,A2,A3)\n"
            "# define AP_LOCAL_MIR_BLOCK 1\n#endif\n"
            "#ifndef ARCHON_MIR_VREG\n"
            "# define ARCHON_MIR_VREG(A0,A1,A2,A3,A4)\n"
            "# define AP_LOCAL_MIR_VREG 1\n#endif\n"
            "#ifndef ARCHON_MIR_INSTRUCTION\n"
            "# define ARCHON_MIR_INSTRUCTION(A0,A1,A2,A3,A4,A5,A6,A7,A8,A9)\n"
            "# define AP_LOCAL_MIR_INSTRUCTION 1\n#endif\n"
            "#ifndef ARCHON_MIR_DEF\n"
            "# define ARCHON_MIR_DEF(A0,A1,A2,A3)\n"
            "# define AP_LOCAL_MIR_DEF 1\n#endif\n"
            "#ifndef ARCHON_MIR_USE\n"
            "# define ARCHON_MIR_USE(A0,A1,A2,A3)\n"
            "# define AP_LOCAL_MIR_USE 1\n#endif\n"
            "#ifndef ARCHON_MIR_IMMEDIATE\n"
            "# define ARCHON_MIR_IMMEDIATE(A0,A1,A2,A3,A4)\n"
            "# define AP_LOCAL_MIR_IMMEDIATE 1\n#endif\n"
            "#ifndef ARCHON_MIR_SUCCESSOR\n"
            "# define ARCHON_MIR_SUCCESSOR(A0,A1,A2,A3)\n"
            "# define AP_LOCAL_MIR_SUCCESSOR 1\n#endif\n"
            "#ifndef ARCHON_MIR_DEPENDENCY\n"
            "# define ARCHON_MIR_DEPENDENCY(A0,A1,A2,A3,A4,A5,A6)\n"
            "# define AP_LOCAL_MIR_DEPENDENCY 1\n#endif\n"
            "#ifndef ARCHON_MIR_END\n"
            "# define ARCHON_MIR_END(A0)\n"
            "# define AP_LOCAL_MIR_END 1\n#endif\n\n",
            output) != EOF;
}

static int AP_mir_epilogue(FILE *output) {
    return fputs(
        "\n#ifdef AP_LOCAL_MIR_END\n# undef ARCHON_MIR_END\n"
        "# undef AP_LOCAL_MIR_END\n#endif\n"
        "#ifdef AP_LOCAL_MIR_DEPENDENCY\n# undef ARCHON_MIR_DEPENDENCY\n"
        "# undef AP_LOCAL_MIR_DEPENDENCY\n#endif\n"
        "#ifdef AP_LOCAL_MIR_SUCCESSOR\n# undef ARCHON_MIR_SUCCESSOR\n"
        "# undef AP_LOCAL_MIR_SUCCESSOR\n#endif\n"
        "#ifdef AP_LOCAL_MIR_IMMEDIATE\n# undef ARCHON_MIR_IMMEDIATE\n"
        "# undef AP_LOCAL_MIR_IMMEDIATE\n#endif\n"
        "#ifdef AP_LOCAL_MIR_USE\n# undef ARCHON_MIR_USE\n"
        "# undef AP_LOCAL_MIR_USE\n#endif\n"
        "#ifdef AP_LOCAL_MIR_DEF\n# undef ARCHON_MIR_DEF\n"
        "# undef AP_LOCAL_MIR_DEF\n#endif\n"
        "#ifdef AP_LOCAL_MIR_INSTRUCTION\n# undef ARCHON_MIR_INSTRUCTION\n"
        "# undef AP_LOCAL_MIR_INSTRUCTION\n#endif\n"
        "#ifdef AP_LOCAL_MIR_VREG\n# undef ARCHON_MIR_VREG\n"
        "# undef AP_LOCAL_MIR_VREG\n#endif\n"
        "#ifdef AP_LOCAL_MIR_BLOCK\n# undef ARCHON_MIR_BLOCK\n"
        "# undef AP_LOCAL_MIR_BLOCK\n#endif\n"
        "#ifdef AP_LOCAL_MIR_BEGIN\n# undef ARCHON_MIR_BEGIN\n"
        "# undef AP_LOCAL_MIR_BEGIN\n#endif\n"
        "#undef ARCHON_MACRO_ABI_ACTIVE_MIR\n",
        output) != EOF;
}

static const char *AP_opcode(const AP_Operation *operation) {
    if (operation->target != NULL) {
        size_t instruction;
        Emitter_Instruction_Info info;
        if (Emitter_lookup_instruction(
                operation->target, &instruction) &&
            Emitter_instruction_info(instruction, &info))
            return info.identifier;
        return "ARCHON_ID_62617272696572";
    }
    if (operation->primitive == NULL)
        return "ARCHON_ID_62617272696572";
    if (strcmp(operation->primitive, "copy") == 0)
        return "ARCHON_ID_636F7079";
    if (strcmp(operation->primitive, "add") == 0)
        return "ARCHON_ID_616464";
    if (strcmp(operation->primitive, "sub") == 0)
        return "ARCHON_ID_737562";
    if (strcmp(operation->primitive, "mul") == 0)
        return "ARCHON_ID_6D756C";
    if (strcmp(operation->primitive, "and") == 0)
        return "ARCHON_ID_616E64";
    if (strcmp(operation->primitive, "or") == 0)
        return "ARCHON_ID_6F72";
    if (strcmp(operation->primitive, "xor") == 0)
        return "ARCHON_ID_786F72";
    if (strcmp(operation->primitive, "not") == 0)
        return "ARCHON_ID_6E6F74";
    if (strcmp(operation->primitive, "neg") == 0)
        return "ARCHON_ID_6E6567";
    if (strcmp(operation->primitive, "shl") == 0)
        return "ARCHON_ID_73686C";
    if (strcmp(operation->primitive, "lshr") == 0)
        return "ARCHON_ID_6C736872";
    if (strcmp(operation->primitive, "ashr") == 0)
        return "ARCHON_ID_61736872";
    return "ARCHON_ID_62617272696572";
}

static size_t AP_nonconstant_values(const AP_Context *context) {
    size_t count = 0u, index;
    for (index = 0u; index < context->value_count; ++index)
        if (!context->values[index].is_constant)
            ++count;
    return count;
}

static size_t AP_register_input_count(const AP_Operation *operation) {
    size_t count = 0u, index;
    for (index = 0u; index < operation->input_count; ++index)
        if (operation->inputs[index].kind == AP_INPUT_VALUE)
            ++count;
    return count;
}

static size_t AP_immediate_input_count(const AP_Operation *operation) {
    size_t count = 0u, index;
    for (index = 0u; index < operation->input_count; ++index)
        if (operation->inputs[index].kind == AP_INPUT_IMMEDIATE)
            ++count;
    return count;
}

static size_t AP_dependencies(
        const AP_Context *context, size_t operation_index,
        size_t *predecessors, unsigned *kinds, size_t capacity) {
    const AP_Operation *operation = &context->operations[operation_index];
    size_t count = 0u, input, candidate;
    for (input = 0u; input < operation->input_count; ++input) {
        if (operation->inputs[input].kind != AP_INPUT_VALUE)
            continue;
        for (candidate = operation_index; candidate-- > 0u;) {
            const AP_Operation *producer = &context->operations[candidate];
            if (producer->has_result &&
                    producer->result == operation->inputs[input].value) {
                size_t existing;
                for (existing = 0u; existing < count; ++existing)
                    if (predecessors[existing] == candidate)
                        break;
                if (existing == count && count < capacity) {
                    predecessors[count] = candidate;
                    kinds[count++] = ARCHON_DEP_DATA;
                }
                break;
            }
        }
    }
    for (candidate = operation_index; candidate-- > 0u;) {
        if (context->operations[candidate].block == operation->block) {
            size_t existing;
            for (existing = 0u; existing < count; ++existing)
                if (predecessors[existing] == candidate)
                    break;
            if (existing == count && count < capacity) {
                predecessors[count] = candidate;
                kinds[count++] = ARCHON_DEP_ORDER;
            }
            break;
        }
    }
    return count;
}

static int AP_emit_mir(AP_Context *context, FILE *output) {
    size_t block, value, operation;
    size_t vreg_ordinal = 0u;
    if (output == NULL)
        return AP_fail(context, "null MIR output stream");
    if (context->block_count != 1u)
        return AP_fail(
            context,
            "canonical MIR CFG lowering is unavailable for %zu blocks",
            context->block_count);
    if (!AP_mir_prologue(output) ||
        fprintf(
            output,
            "ARCHON_MIR_BEGIN(ARCHON_MACRO_ABI_VERSION, "
            "ARCHON_MIR_SCHEMA_VERSION, UINT64_C(0x%016" PRIX64 "), "
            "UINT64_C(0x%016" PRIX64 "), UINT32_C(%zu), UINT32_C(%zu), "
            "UINT32_C(%zu))\n",
            context->mir_stream_id, context->program_stream_id,
            context->block_count, AP_nonconstant_values(context),
            context->operation_count) < 0)
        goto failed;
    for (block = 0u; block < context->block_count; ++block) {
        if (fprintf(
                output,
                "ARCHON_MIR_BLOCK(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%zu), ",
                (uint64_t)block + UINT64_C(1), block) < 0 ||
            !AP_write_string(output, context->blocks[block].name) ||
            fputs(", UINT32_C(0))\n", output) == EOF)
            goto failed;
    }
    for (value = 0u; value < context->value_count; ++value) {
        const AP_Value *row = &context->values[value];
        if (row->is_constant)
            continue;
        if (fprintf(
                output,
                "ARCHON_MIR_VREG(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%zu), ARCHON_ID_67656E65726963, "
                "UINT32_C(%u), UINT32_C(0))\n",
                (uint64_t)row->id + UINT64_C(1), vreg_ordinal++,
                row->width) < 0)
            goto failed;
    }
    for (block = 0u; block < context->block_count; ++block) {
        for (operation = 0u;
             operation < context->operation_count; ++operation) {
            const AP_Operation *row = &context->operations[operation];
            size_t predecessors[66];
            unsigned kinds[66];
            size_t dependency_count;
            size_t ordinal;
            if (row->block != context->blocks[block].id)
                continue;
            dependency_count = AP_dependencies(
                context, operation, predecessors, kinds,
                sizeof predecessors / sizeof predecessors[0]);
            ordinal = AP_operation_ordinal(context, operation);
            if (fprintf(
                    output,
                    "ARCHON_MIR_INSTRUCTION(UINT64_C(0x%016" PRIX64 "), "
                    "UINT64_C(0x%016" PRIX64 "), UINT32_C(%zu), %s, "
                    "UINT32_C(%u), UINT32_C(%zu), UINT32_C(%zu), "
                    "UINT32_C(0), UINT32_C(%zu), UINT32_C(%u))\n",
                    (uint64_t)row->id + UINT64_C(1),
                    AP_mir_block_id(context, row->block), ordinal,
                    AP_opcode(row), row->has_result,
                    AP_register_input_count(row),
                    AP_immediate_input_count(row), dependency_count,
                    row->barrier || row->unsupported != NULL) < 0)
                goto failed;
        }
    }
    for (operation = 0u; operation < context->operation_count; ++operation) {
        const AP_Operation *row = &context->operations[operation];
        if (row->has_result &&
            fprintf(
                output,
                "ARCHON_MIR_DEF(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(0), UINT64_C(0x%016" PRIX64 "), "
                "ARCHON_ID_67656E65726963)\n",
                (uint64_t)row->id + UINT64_C(1),
                 (uint64_t)row->result + UINT64_C(1)) < 0)
            goto failed;
    }
    for (operation = 0u; operation < context->operation_count; ++operation) {
        const AP_Operation *row = &context->operations[operation];
        size_t index, use_index = 0u;
        for (index = 0u; index < row->input_count; ++index) {
            const AP_Input *input = &row->inputs[index];
            if (input->kind == AP_INPUT_VALUE) {
                if (fprintf(
                        output,
                        "ARCHON_MIR_USE(UINT64_C(0x%016" PRIX64 "), "
                        "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), "
                        "ARCHON_ID_67656E65726963)\n",
                        (uint64_t)row->id + UINT64_C(1), use_index++,
                         (uint64_t)input->value + UINT64_C(1)) < 0)
                    goto failed;
            } else if (input->kind == AP_INPUT_SYMBOL) {
                goto unsupported;
            }
        }
    }
    for (operation = 0u; operation < context->operation_count; ++operation) {
        const AP_Operation *row = &context->operations[operation];
        size_t index, immediate_index = 0u;
        for (index = 0u; index < row->input_count; ++index) {
            const AP_Input *input = &row->inputs[index];
            if (input->kind == AP_INPUT_IMMEDIATE) {
                if (fprintf(
                        output,
                        "ARCHON_MIR_IMMEDIATE(UINT64_C(0x%016" PRIX64 "), "
                        "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), "
                        "UINT32_C(%u), %s)\n",
                        (uint64_t)row->id + UINT64_C(1),
                        immediate_index++, (uint64_t)input->immediate,
                         input->width != 0u ? input->width : AP_WORD_WIDTH,
                         input->is_signed ? "1" : "0") < 0)
                    goto failed;
            } else if (input->kind == AP_INPUT_SYMBOL) {
                goto unsupported;
            }
        }
    }
    for (operation = 0u; operation < context->operation_count; ++operation) {
        const AP_Operation *row = &context->operations[operation];
        size_t predecessors[66];
        unsigned kinds[66];
        size_t index;
        size_t dependency_count = AP_dependencies(
            context, operation, predecessors, kinds,
            sizeof predecessors / sizeof predecessors[0]);
        for (index = 0u; index < dependency_count; ++index)
            if (fprintf(
                    output,
                    "ARCHON_MIR_DEPENDENCY(UINT64_C(0x%016" PRIX64 "), "
                    "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), %s, "
                    "ARCHON_LATENCY_UNKNOWN, UINT32_C(0), UINT32_C(0))\n",
                    (uint64_t)row->id + UINT64_C(1), index,
                    (uint64_t)context->operations[
                        predecessors[index]].id + UINT64_C(1),
                    kinds[index] == ARCHON_DEP_DATA
                        ? "ARCHON_DEP_DATA" : "ARCHON_DEP_ORDER") < 0)
                goto failed;
    }
    if (fprintf(
            output, "ARCHON_MIR_END(UINT64_C(0x%016" PRIX64 "))\n",
            context->mir_stream_id) < 0 ||
        !AP_mir_epilogue(output))
        goto failed;
    return 1;

unsupported:
    (void)AP_fail(context, "MIR symbol operands are not representable");
failed:
    if (context->diagnostic[0] == '\0')
        (void)AP_fail(context, "cannot write MIR stream");
    return 0;
}

static int AP_write_mir(AP_Context *context) {
    FILE *output = fopen("MIR.i", "wb");
    int success;
    if (output == NULL)
        return AP_fail(context, "cannot create MIR.i");
    success = AP_emit_mir(context, output);
    if (fclose(output) != 0)
        success = AP_fail(context, "cannot close MIR.i");
@ORACLE_PROCESS_MIR@
    if (!success)
        (void)remove("MIR.i");
    return success;
}

static int AP_immediate_fits(int64_t value, unsigned width,
                             unsigned is_signed) {
    if (width == 0u || width >= 64u)
        return 1;
    if (is_signed) {
        int64_t minimum = -(INT64_C(1) << (width - 1u));
        int64_t maximum = (INT64_C(1) << (width - 1u)) - INT64_C(1);
        return value >= minimum && value <= maximum;
    }
    return value >= 0 &&
           (uint64_t)value < (UINT64_C(1) << width);
}

static int64_t AP_encoding_immediate(
        const AP_Operation *operation, const AP_Input *input,
        const Emitter_Operand_Info *operand) {
    /* Generic integer arithmetic consumes a bit pattern. The selector has
     * already checked that a sign-extended immediate produces these bits,
     * even for an unsigned source type. Explicit target immediates instead
     * keep their numeric interpretation and must satisfy the operand range. */
    if (operation->target == NULL && operand->is_signed &&
            operand->width != 0u && input->width >= operand->width)
        return AP_signed_bits((uint64_t)input->immediate, operand->width, 1u);
    return input->immediate;
}

static int AP_validate_selection(
        AP_Context *context, const AP_Operation *operation,
        size_t instruction) {
    Emitter_Instruction_Info info;
    size_t operand, input = 0u;
    unsigned outputs = 0u;
    if (!Emitter_instruction_info(instruction, &info))
        return AP_fail(context, "selected instruction metadata is missing");
    if (AP_STACK_TARGET && !info.semantics_known)
        return AP_fail(
            context, "%s is encoding-only on %s; stack semantics are not modeled",
            info.name, Emitter_target_name());
    for (operand = 0u; operand < info.operand_count; ++operand) {
        Emitter_Operand_Info expected;
        const AP_Input *actual;
        if (!Emitter_operand_info(instruction, operand, &expected))
            return AP_fail(context, "selected operand metadata is missing");
        if (expected.role == EMITTER_ROLE_OUTPUT) {
            ++outputs;
            continue;
        }
        if (expected.role != EMITTER_ROLE_INPUT)
            return AP_fail(
                context, "tied operands require target-specific lowering");
        if (input >= operation->input_count)
            return AP_fail(context, "selected instruction has too few inputs");
        actual = &operation->inputs[input++];
        if ((expected.kind == EMITTER_OPERAND_REGISTER &&
                actual->kind != AP_INPUT_VALUE) ||
            (expected.kind == EMITTER_OPERAND_IMMEDIATE &&
                actual->kind != AP_INPUT_IMMEDIATE))
            return AP_fail(context, "selected operand kind mismatch");
        if (actual->kind == AP_INPUT_IMMEDIATE &&
                !AP_immediate_fits(
                    AP_encoding_immediate(operation, actual, &expected),
                    expected.width,
                    expected.is_signed))
            return AP_fail(context, "selected immediate is out of range");
    }
    if (input != operation->input_count ||
            outputs != (operation->has_result ? 1u : 0u))
        return AP_fail(context, "selected instruction arity mismatch");
    return 1;
}

static int AP_instr_map_prologue(FILE *output) {
    return AP_write_common_header(
               output, "InstrMap.i", "ARCHON_INSTR_MAP_SCHEMA_VERSION") &&
        fputs(
            "#ifdef ARCHON_MACRO_ABI_ACTIVE_INSTR_MAP\n"
            "# error \"recursive inclusion of InstrMap.i\"\n"
            "#endif\n"
            "#define ARCHON_MACRO_ABI_ACTIVE_INSTR_MAP 1\n\n"
            "#ifndef ARCHON_INSTR_MAP_BEGIN\n"
            "# define ARCHON_INSTR_MAP_BEGIN(A0,A1,A2,A3,A4,A5)\n"
            "# define AP_LOCAL_IM_BEGIN 1\n#endif\n"
            "#ifndef ARCHON_INSTR_MAP_ENTRY\n"
            "# define ARCHON_INSTR_MAP_ENTRY(A0,A1,A2,A3,A4,A5,A6)\n"
            "# define AP_LOCAL_IM_ENTRY 1\n#endif\n"
            "#ifndef ARCHON_INSTR_MAP_END\n"
            "# define ARCHON_INSTR_MAP_END(A0)\n"
            "# define AP_LOCAL_IM_END 1\n#endif\n\n",
            output) != EOF;
}

static int AP_instr_map_epilogue(FILE *output) {
    return fputs(
        "\n#ifdef AP_LOCAL_IM_END\n# undef ARCHON_INSTR_MAP_END\n"
        "# undef AP_LOCAL_IM_END\n#endif\n"
        "#ifdef AP_LOCAL_IM_ENTRY\n# undef ARCHON_INSTR_MAP_ENTRY\n"
        "# undef AP_LOCAL_IM_ENTRY\n#endif\n"
        "#ifdef AP_LOCAL_IM_BEGIN\n# undef ARCHON_INSTR_MAP_BEGIN\n"
        "# undef AP_LOCAL_IM_BEGIN\n#endif\n"
        "#undef ARCHON_MACRO_ABI_ACTIVE_INSTR_MAP\n",
        output) != EOF;
}

static int AP_select(AP_Context *context) {
    FILE *map = NULL, *selected = NULL;
    size_t operation;
@ORACLE_SELECT_PREPARE@
    if (context->block_count != 1u)
        return AP_fail(
            context,
            "CFG lowering is unavailable for %zu source blocks",
            context->block_count);
    context->selected = (AP_Selected *)calloc(
        context->operation_count != 0u ? context->operation_count : 1u,
        sizeof *context->selected);
    if (context->selected == NULL)
        return AP_fail(context, "out of memory during instruction selection");
    for (operation = 0u; operation < context->operation_count; ++operation) {
        AP_Operation *source = &context->operations[operation];
        ArchonISelOperand operands[64];
        ArchonISelOperation request;
        ArchonISelChoice choice;
        enum ArchonISelStatus status;
        char message[160];
        size_t instruction, input;
        if (source->unsupported != NULL)
            return AP_fail(
                context, "unsupported operation barrier: %s",
                source->unsupported);
        if (AP_STACK_TARGET && source->target == NULL)
            return AP_fail(
                context,
                "%s requires explicit target stack operations; "
                "SSA-to-stack lowering is unavailable",
                Emitter_target_name());
        if (source->input_count > sizeof operands / sizeof operands[0])
            return AP_fail(context, "operation has too many operands");
        for (input = 0u; input < source->input_count; ++input) {
            const AP_Input *value = &source->inputs[input];
            operands[input].width = value->width != 0u
                ? value->width : AP_WORD_WIDTH;
            operands[input].is_signed = value->is_signed;
            if (value->kind == AP_INPUT_VALUE) {
                operands[input].kind = ARCHON_ISEL_OPERAND_VALUE;
                operands[input].bits = value->value;
            } else if (value->kind == AP_INPUT_IMMEDIATE) {
                operands[input].kind = ARCHON_ISEL_OPERAND_CONSTANT;
                operands[input].bits = (uint64_t)value->immediate;
            } else {
                operands[input].kind = ARCHON_ISEL_OPERAND_SYMBOL;
                operands[input].bits = UINT64_C(0);
            }
        }
        memset(&request, 0, sizeof request);
        request.operation = source->target == NULL
            ? source->primitive : NULL;
        request.explicit_target = source->target;
        request.width = source->result_width;
        request.effects = source->barrier ? 1u : 0u;
        request.operands = operands;
        request.operand_count = source->input_count;
@ORACLE_SELECT_CALL@
        if (status != ARCHON_ISEL_OK) {
            return AP_fail(
                context,
                "e-graph instruction selection failed for %s: %s",
                source->target != NULL ? source->target :
                source->primitive != NULL ? source->primitive :
                "opaque operation",
                message[0] != '\0'
                    ? message : archon_isel_status_string(status));
        }
        if (!Emitter_lookup_instruction(choice.target, &instruction))
            return AP_fail(
                context,
                "e-graph selector chose unknown target instruction %s",
                choice.target != NULL ? choice.target : "<null>");
        if (!AP_validate_selection(context, source, instruction))
            return 0;
        context->selected[context->selected_count].operation = operation;
        context->selected[context->selected_count].instruction = instruction;
        ++context->selected_count;
    }
    if (context->selected_count == 0u)
        return AP_fail(context, "program contains no selectable operations");

    map = fopen("InstrMap.i", "wb");
    selected = fopen("Selected.i", "wb");
    if (map == NULL || selected == NULL)
        goto failed;
    if (!AP_instr_map_prologue(map) ||
        fprintf(
            map,
            "ARCHON_INSTR_MAP_BEGIN(ARCHON_MACRO_ABI_VERSION, "
            "ARCHON_INSTR_MAP_SCHEMA_VERSION, UINT64_C(0x%016" PRIX64 "), "
            "UINT64_C(0x%016" PRIX64 "), UINT64_C(0x%016" PRIX64 "), "
            "UINT32_C(%zu))\n",
            context->instr_map_stream_id, context->program_stream_id,
            context->mir_stream_id, context->selected_count) < 0 ||
        fprintf(
            selected, "ARCHON_SELECTED_BEGIN(%zu)\n",
            context->selected_count) < 0)
        goto failed;
    for (operation = 0u; operation < context->selected_count; ++operation) {
        AP_Selected *row = &context->selected[operation];
        AP_Operation *source = &context->operations[row->operation];
        Emitter_Instruction_Info info;
        size_t input;
        if (!Emitter_instruction_info(row->instruction, &info) ||
            fprintf(
                map,
                "ARCHON_INSTR_MAP_ENTRY(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), %s, "
                "UINT32_C(%zu), ARCHON_MAP_PRIMARY, UINT32_C(0))\n",
                (uint64_t)row->operation + UINT64_C(1), operation,
                (uint64_t)row->operation + UINT64_C(1),
                info.identifier, operation) < 0 ||
            fprintf(
                selected, "ARCHON_SELECTED_OP(%zu, ", operation) < 0 ||
            !AP_write_string(selected, info.name) ||
            fprintf(
                selected, ", %" PRIu32 ", %zu)\n",
                source->has_result ? source->result : UINT32_MAX,
                source->input_count) < 0)
            goto failed;
        for (input = 0u; input < source->input_count; ++input) {
            const AP_Input *value = &source->inputs[input];
            if (value->kind == AP_INPUT_VALUE) {
                if (fprintf(
                        selected,
                        "ARCHON_SELECTED_VALUE(%zu, %" PRIu32 ")\n",
                        operation, value->value) < 0)
                    goto failed;
            } else if (value->kind == AP_INPUT_IMMEDIATE) {
                if (fprintf(
                        selected,
                        "ARCHON_SELECTED_IMMEDIATE(%zu, %" PRId64 ")\n",
                        operation, value->immediate) < 0)
                    goto failed;
            } else {
                if (fprintf(
                        selected, "ARCHON_SELECTED_SYMBOL(%zu, ",
                        operation) < 0 ||
                    !AP_write_string(selected, value->symbol) ||
                    fputs(")\n", selected) == EOF)
                    goto failed;
            }
        }
    }
    if (fprintf(
            map,
            "ARCHON_INSTR_MAP_END(UINT64_C(0x%016" PRIX64 "))\n",
            context->instr_map_stream_id) < 0 ||
        !AP_instr_map_epilogue(map) ||
        fputs("ARCHON_SELECTED_END()\n", selected) == EOF)
        goto failed;
    {
        int map_close = fclose(map);
        int selected_close;
        map = NULL;
        selected_close = fclose(selected);
        selected = NULL;
        if (map_close != 0 || selected_close != 0)
            goto closed_failed;
    }
    return 1;

failed:
    if (map != NULL)
        (void)fclose(map);
    if (selected != NULL)
        (void)fclose(selected);
closed_failed:
    (void)remove("InstrMap.i");
    (void)remove("Selected.i");
    return AP_fail(context, "cannot write instruction-selection artifacts");
}

static int AP_scheduled_prologue(FILE *output) {
    return AP_write_common_header(
               output, "Scheduled.i", "ARCHON_SCHEDULED_SCHEMA_VERSION") &&
        fputs(
            "#ifdef ARCHON_MACRO_ABI_ACTIVE_SCHEDULED\n"
            "# error \"recursive inclusion of Scheduled.i\"\n"
            "#endif\n"
            "#define ARCHON_MACRO_ABI_ACTIVE_SCHEDULED 1\n\n"
            "#ifndef ARCHON_SCHEDULED_BEGIN\n"
            "# define ARCHON_SCHEDULED_BEGIN(A0,A1,A2,A3,A4,A5,A6)\n"
            "# define AP_LOCAL_S_BEGIN 1\n#endif\n"
            "#ifndef ARCHON_SCHEDULED_REGION\n"
            "# define ARCHON_SCHEDULED_REGION(A0,A1,A2,A3,A4,A5)\n"
            "# define AP_LOCAL_S_REGION 1\n#endif\n"
            "#ifndef ARCHON_SCHEDULED_INSTRUCTION\n"
            "# define ARCHON_SCHEDULED_INSTRUCTION(A0,A1,A2,A3,A4,A5,A6,A7)\n"
            "# define AP_LOCAL_S_INSTRUCTION 1\n#endif\n"
            "#ifndef ARCHON_SCHEDULED_END\n"
            "# define ARCHON_SCHEDULED_END(A0)\n"
            "# define AP_LOCAL_S_END 1\n#endif\n\n",
            output) != EOF;
}

static int AP_scheduled_epilogue(FILE *output) {
    return fputs(
        "\n#ifdef AP_LOCAL_S_END\n# undef ARCHON_SCHEDULED_END\n"
        "# undef AP_LOCAL_S_END\n#endif\n"
        "#ifdef AP_LOCAL_S_INSTRUCTION\n"
        "# undef ARCHON_SCHEDULED_INSTRUCTION\n"
        "# undef AP_LOCAL_S_INSTRUCTION\n#endif\n"
        "#ifdef AP_LOCAL_S_REGION\n# undef ARCHON_SCHEDULED_REGION\n"
        "# undef AP_LOCAL_S_REGION\n#endif\n"
        "#ifdef AP_LOCAL_S_BEGIN\n# undef ARCHON_SCHEDULED_BEGIN\n"
        "# undef AP_LOCAL_S_BEGIN\n#endif\n"
        "#undef ARCHON_MACRO_ABI_ACTIVE_SCHEDULED\n",
        output) != EOF;
}

static int AP_reg_map_prologue(FILE *output) {
    return AP_write_common_header(
               output, "RegMap.i", "ARCHON_REG_MAP_SCHEMA_VERSION") &&
        fputs(
            "#ifdef ARCHON_MACRO_ABI_ACTIVE_REG_MAP\n"
            "# error \"recursive inclusion of RegMap.i\"\n"
            "#endif\n"
            "#define ARCHON_MACRO_ABI_ACTIVE_REG_MAP 1\n\n"
            "#ifndef ARCHON_REG_MAP_BEGIN\n"
            "# define ARCHON_REG_MAP_BEGIN(A0,A1,A2,A3,A4,A5,A6,A7,A8,A9,A10,A11,A12)\n"
            "# define AP_LOCAL_RM_BEGIN 1\n#endif\n"
            "#ifndef ARCHON_REG_MAP_REGISTER\n"
            "# define ARCHON_REG_MAP_REGISTER(A0,A1,A2,A3,A4,A5)\n"
            "# define AP_LOCAL_RM_REGISTER 1\n#endif\n"
            "#ifndef ARCHON_REG_MAP_ALIAS\n"
            "# define ARCHON_REG_MAP_ALIAS(A0,A1,A2,A3,A4,A5,A6)\n"
            "# define AP_LOCAL_RM_ALIAS 1\n#endif\n"
            "#ifndef ARCHON_REG_MAP_RESERVED\n"
            "# define ARCHON_REG_MAP_RESERVED(A0,A1,A2,A3,A4)\n"
            "# define AP_LOCAL_RM_RESERVED 1\n#endif\n"
            "#ifndef ARCHON_REG_MAP_VREG\n"
            "# define ARCHON_REG_MAP_VREG(A0,A1,A2,A3,A4)\n"
            "# define AP_LOCAL_RM_VREG 1\n#endif\n"
            "#ifndef ARCHON_REG_MAP_INTERVAL\n"
            "# define ARCHON_REG_MAP_INTERVAL(A0,A1,A2,A3,A4,A5)\n"
            "# define AP_LOCAL_RM_INTERVAL 1\n#endif\n"
            "#ifndef ARCHON_REG_MAP_EVENT\n"
            "# define ARCHON_REG_MAP_EVENT(A0,A1,A2,A3,A4,A5,A6)\n"
            "# define AP_LOCAL_RM_EVENT 1\n#endif\n"
            "#ifndef ARCHON_REG_MAP_FIXED\n"
            "# define ARCHON_REG_MAP_FIXED(A0,A1,A2,A3,A4)\n"
            "# define AP_LOCAL_RM_FIXED 1\n#endif\n"
            "#ifndef ARCHON_REG_MAP_END\n"
            "# define ARCHON_REG_MAP_END(A0)\n"
            "# define AP_LOCAL_RM_END 1\n#endif\n\n",
            output) != EOF;
}

static int AP_reg_map_epilogue(FILE *output) {
    return fputs(
        "\n#ifdef AP_LOCAL_RM_END\n# undef ARCHON_REG_MAP_END\n"
        "# undef AP_LOCAL_RM_END\n#endif\n"
        "#ifdef AP_LOCAL_RM_FIXED\n# undef ARCHON_REG_MAP_FIXED\n"
        "# undef AP_LOCAL_RM_FIXED\n#endif\n"
        "#ifdef AP_LOCAL_RM_EVENT\n# undef ARCHON_REG_MAP_EVENT\n"
        "# undef AP_LOCAL_RM_EVENT\n#endif\n"
        "#ifdef AP_LOCAL_RM_INTERVAL\n# undef ARCHON_REG_MAP_INTERVAL\n"
        "# undef AP_LOCAL_RM_INTERVAL\n#endif\n"
        "#ifdef AP_LOCAL_RM_VREG\n# undef ARCHON_REG_MAP_VREG\n"
        "# undef AP_LOCAL_RM_VREG\n#endif\n"
        "#ifdef AP_LOCAL_RM_RESERVED\n# undef ARCHON_REG_MAP_RESERVED\n"
        "# undef AP_LOCAL_RM_RESERVED\n#endif\n"
        "#ifdef AP_LOCAL_RM_ALIAS\n# undef ARCHON_REG_MAP_ALIAS\n"
        "# undef AP_LOCAL_RM_ALIAS\n#endif\n"
        "#ifdef AP_LOCAL_RM_REGISTER\n# undef ARCHON_REG_MAP_REGISTER\n"
        "# undef AP_LOCAL_RM_REGISTER\n#endif\n"
        "#ifdef AP_LOCAL_RM_BEGIN\n# undef ARCHON_REG_MAP_BEGIN\n"
        "# undef AP_LOCAL_RM_BEGIN\n#endif\n"
        "#undef ARCHON_MACRO_ABI_ACTIVE_REG_MAP\n",
        output) != EOF;
}

static int AP_write_reg_map(AP_Context *context) {
    FILE *output = fopen("RegMap.i", "wb");
    size_t index, alias, reserved = 0u, vreg = 0u, fixed = 0u;
    if (output == NULL)
        return AP_fail(context, "cannot create RegMap.i");
    for (index = 0u; index < context->allocator.register_count; ++index)
        if (context->allocator.registers[index].reserved)
            ++reserved;
    for (index = 0u; index < context->value_count; ++index)
        if (!context->values[index].is_constant)
            ++vreg;
    for (index = 0u; index < context->allocator.interval_count; ++index)
        if (context->allocator.intervals[index].fixed)
            ++fixed;
    if (!AP_reg_map_prologue(output) ||
        fprintf(
            output,
            "ARCHON_REG_MAP_BEGIN(ARCHON_MACRO_ABI_VERSION, "
            "ARCHON_REG_MAP_SCHEMA_VERSION, UINT64_C(0x%016" PRIX64 "), ",
            context->reg_map_stream_id) < 0 ||
        !AP_write_string(output, Emitter_target_name()) ||
        fprintf(
            output,
            ", UINT64_C(0x%016" PRIX64 "), UINT64_C(0x%016" PRIX64 "), "
            "UINT32_C(%zu), UINT32_C(%zu), UINT32_C(%zu), "
            "UINT32_C(%zu), UINT32_C(%zu), UINT32_C(%zu), "
            "UINT32_C(%zu))\n",
            context->scheduled_stream_id, context->mir_stream_id,
            context->allocator.register_count,
            context->allocator.alias_count, reserved, vreg,
            context->allocator.interval_count,
            context->allocator.event_count, fixed) < 0)
        goto failed;
    for (index = 0u; index < context->allocator.register_count; ++index) {
        const struct RA_register *row = &context->allocator.registers[index];
        if (fprintf(
                output,
                "ARCHON_REG_MAP_REGISTER(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%zu), ",
                (uint64_t)row->id + UINT64_C(1), index) < 0 ||
            !AP_write_string(output, row->name) ||
            fprintf(
                output,
                ", ARCHON_ID_67656E65726963, UINT32_C(%u), "
                "UINT32_C(%u))\n",
                AP_WORD_WIDTH, row->hardwired_zero) < 0)
            goto failed;
    }
    for (alias = 0u; alias < context->allocator.alias_count; ++alias) {
        const struct RA_alias *row = &context->allocator.aliases[alias];
        if (fprintf(
                output,
                "ARCHON_REG_MAP_ALIAS(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%zu), ",
                (uint64_t)alias + UINT64_C(1), alias) < 0 ||
            !AP_write_string(output, row->name) ||
            fprintf(
                output,
                ", UINT64_C(0x%016" PRIX64 "), UINT32_C(0), "
                "UINT32_C(%u), ARCHON_WRITE_PRESERVE)\n",
                (uint64_t)row->canonical + UINT64_C(1),
                AP_WORD_WIDTH) < 0)
            goto failed;
    }
    {
        size_t ordinal = 0u;
        for (index = 0u; index < context->allocator.register_count; ++index) {
            const struct RA_register *row =
                &context->allocator.registers[index];
            if (!row->reserved)
                continue;
            if (fprintf(
                    output,
                    "ARCHON_REG_MAP_RESERVED(UINT64_C(0x%016" PRIX64 "), "
                    "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), ",
                    (uint64_t)ordinal + UINT64_C(1), ordinal,
                    (uint64_t)row->id + UINT64_C(1)) < 0 ||
                !AP_write_string(
                    output, row->hardwired_zero
                        ? "hardwired zero" : "reserved") ||
                fputs(", UINT32_C(0))\n", output) == EOF)
                goto failed;
            ++ordinal;
        }
    }
    {
        size_t ordinal = 0u;
        for (index = 0u; index < context->value_count; ++index) {
            const AP_Value *row = &context->values[index];
            if (row->is_constant)
                continue;
            if (fprintf(
                    output,
                    "ARCHON_REG_MAP_VREG(UINT64_C(0x%016" PRIX64 "), "
                    "UINT32_C(%zu), ARCHON_ID_67656E65726963, "
                    "UINT32_C(%u), UINT32_C(0))\n",
                    (uint64_t)row->id + UINT64_C(1), ordinal++,
                    row->width) < 0)
                goto failed;
        }
    }
    for (index = 0u; index < context->allocator.interval_count; ++index) {
        const struct RA_interval *row = &context->allocator.intervals[index];
        uint64_t begin = UINT64_MAX, end = 0u;
        size_t range, event_index;
        for (range = 0u; range < row->range_count; ++range) {
            if (row->ranges[range].begin < begin)
                begin = row->ranges[range].begin;
            if (row->ranges[range].end > end)
                end = row->ranges[range].end;
        }
        for (event_index = 0u;
             event_index < context->allocator.event_count; ++event_index) {
            const struct RA_event *event =
                &context->allocator.events[event_index];
            if (event->value != row->value)
                continue;
            if (event->point < begin)
                begin = event->point;
            if (event->point != UINT64_MAX &&
                    event->point + UINT64_C(1) > end)
                end = event->point + UINT64_C(1);
        }
        if (begin == UINT64_MAX) {
            begin = 0u;
            end = 1u;
        } else if (end <= begin) {
            end = begin != UINT64_MAX
                ? begin + UINT64_C(1) : begin;
        }
        if (fprintf(
                output,
                "ARCHON_REG_MAP_INTERVAL(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), "
                "UINT64_C(0x%016" PRIX64 "), "
                "UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(0))\n",
                (uint64_t)index + UINT64_C(1), index,
                (uint64_t)row->value, begin, end) < 0)
            goto failed;
    }
    for (index = 0u; index < context->allocator.event_count; ++index) {
        const struct RA_event *row = &context->allocator.events[index];
        size_t position = row->point;
        unsigned operation = position < context->schedule.operation_count
            ? context->schedule.order[position] : 0u;
        if (fprintf(
                output,
                "ARCHON_REG_MAP_EVENT(UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), "
                "UINT64_C(0x%016" PRIX64 "), %s, "
                "UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(0))\n",
                (uint64_t)index + UINT64_C(1), index,
                (uint64_t)operation + UINT64_C(1),
                (uint64_t)row->point,
                row->definition
                    ? "ARCHON_REG_EVENT_DEF" : "ARCHON_REG_EVENT_USE",
                (uint64_t)row->value) < 0)
            goto failed;
    }
    {
        size_t ordinal = 0u;
        for (index = 0u; index < context->allocator.interval_count; ++index) {
            const struct RA_interval *row =
                &context->allocator.intervals[index];
            if (!row->fixed)
                continue;
            if (fprintf(
                    output,
                    "ARCHON_REG_MAP_FIXED(UINT64_C(0x%016" PRIX64 "), "
                    "UINT32_C(%zu), UINT64_C(0x%016" PRIX64 "), "
                    "UINT64_C(0x%016" PRIX64 "), UINT32_C(0))\n",
                    (uint64_t)ordinal + UINT64_C(1), ordinal,
                    (uint64_t)row->value,
                    (uint64_t)row->fixed_register + UINT64_C(1)) < 0)
                goto failed;
            ++ordinal;
        }
    }
    if (fprintf(
            output,
            "ARCHON_REG_MAP_END(UINT64_C(0x%016" PRIX64 "))\n",
            context->reg_map_stream_id) < 0 ||
            !AP_reg_map_epilogue(output))
        goto failed;
    if (fclose(output) != 0) {
        output = NULL;
        goto closed_failed;
    }
    output = NULL;
    return 1;

failed:
    if (output != NULL)
        (void)fclose(output);
closed_failed:
    (void)remove("RegMap.i");
    return AP_fail(context, "cannot write RegMap.i");
}

static int AP_schedule(AP_Context *context) {
    ISched_Options options = isched_default_options();
    enum ISched_Status status;
    FILE *output = NULL;
    size_t operation, maximum_dependencies =
        context->selected_count != 0u ? context->selected_count - 1u : 0u;
@ORACLE_ISCHED_PREPARE@
    context->schedule_operations = (ISched_Operation *)calloc(
        context->selected_count, sizeof *context->schedule_operations);
    context->dependencies = (ISched_Dependency *)calloc(
        maximum_dependencies != 0u ? maximum_dependencies : 1u,
        sizeof *context->dependencies);
    if (context->schedule_operations == NULL ||
            context->dependencies == NULL)
        return AP_fail(context, "out of memory during instruction scheduling");
    for (operation = 0u; operation < context->selected_count; ++operation) {
        Emitter_Instruction_Info info;
        AP_Operation *source =
            &context->operations[context->selected[operation].operation];
        ISched_Operation *row = &context->schedule_operations[operation];
        if (!Emitter_instruction_info(
                context->selected[operation].instruction, &info))
            return AP_fail(context, "missing selected instruction metadata");
        row->id = (unsigned)operation;
        row->opcode = info.name;
        row->resource = 0u;
        /*
         * Unit values are solver surrogates under a conservative barrier
         * model.  MIR.i records latency as explicitly unknown and the
         * Scheduled.i producer is FALLBACK, so these are never advertised as
         * microarchitectural facts.
         */
        row->latency = 1u;
        row->occupancy = 1u;
        row->units = 1u;
        row->pressure_delta = source->has_result ? 1 : 0;
        row->register_class = 0u;
        if (operation != 0u) {
            ISched_Dependency *dependency =
                &context->dependencies[context->dependency_count++];
            dependency->from = (unsigned)(operation - 1u);
            dependency->to = (unsigned)operation;
            dependency->latency = 1u;
            dependency->distance = 0u;
            dependency->flags = ISCHED_DEP_ORDER;
            dependency->register_class = ISCHED_NO_REGISTER_CLASS;
            dependency->value = 0u;
        }
    }
    context->resource.name = "conservative-unknown";
    context->resource.capacity = 1u;
    context->resource.flags = ISCHED_MODEL_CONSERVATIVE;
    context->problem.operations = context->schedule_operations;
    context->problem.operation_count = context->selected_count;
    context->problem.dependencies = context->dependencies;
    context->problem.dependency_count = context->dependency_count;
    context->problem.resources = &context->resource;
    context->problem.resource_count = 1u;
@ORACLE_ISCHED_CALL@
    if (status != ISCHED_STATUS_SAT &&
            status != ISCHED_STATUS_FALLBACK)
        return AP_fail(
            context, "instruction scheduler failed with status %u",
            (unsigned)status);

    output = fopen("Scheduled.i", "wb");
    if (output == NULL || !AP_scheduled_prologue(output) ||
        fprintf(
            output,
            "ARCHON_SCHEDULED_BEGIN(ARCHON_MACRO_ABI_VERSION, "
            "ARCHON_SCHEDULED_SCHEMA_VERSION, UINT64_C(0x%016" PRIX64 "), "
            "UINT64_C(0x%016" PRIX64 "), UINT64_C(0x%016" PRIX64 "), "
            "UINT32_C(1), UINT32_C(%zu))\n"
            "ARCHON_SCHEDULED_REGION(UINT64_C(0x0000000000000001), "
            "UINT32_C(0), UINT64_C(0x0000000000000001), UINT32_C(%u), "
            "ARCHON_SCHEDULE_FALLBACK, UINT32_C(1))\n",
            context->scheduled_stream_id, context->mir_stream_id,
            context->instr_map_stream_id, context->selected_count,
            context->schedule.ii != 0u ? context->schedule.ii : 1u) < 0)
        goto failed;
    for (operation = 0u; operation < context->selected_count; ++operation) {
        unsigned selected_operation = context->schedule.order[operation];
        unsigned source_operation = (unsigned)
            context->selected[selected_operation].operation;
        if (fprintf(
                output,
                "ARCHON_SCHEDULED_INSTRUCTION("
                "UINT64_C(0x%016" PRIX64 "), UINT32_C(%zu), "
                "UINT64_C(0x%016" PRIX64 "), "
                "UINT64_C(0x0000000000000001), "
                "UINT64_C(0x0000000000000001), "
                "UINT64_C(0x%016" PRIX64 "), "
                "UINT32_C(%u), UINT32_C(1))\n",
                (uint64_t)source_operation + UINT64_C(1), operation,
                (uint64_t)source_operation + UINT64_C(1),
                (uint64_t)context->schedule.cycle[selected_operation],
                context->schedule.slot[selected_operation]) < 0)
            goto failed;
    }
    if (fprintf(
            output,
            "ARCHON_SCHEDULED_END(UINT64_C(0x%016" PRIX64 "))\n",
            context->scheduled_stream_id) < 0 ||
            !AP_scheduled_epilogue(output))
        goto failed;
    if (fclose(output) != 0) {
        output = NULL;
        goto closed_failed;
    }
    output = NULL;
@ORACLE_PROCESS_SCHEDULED@

    RA_init(&context->allocator);
    context->allocator_initialized = 1u;
    /* Physical indices are zero-based; canonical artifact IDs aren't. */
    for (operation = 0u; operation < context->allocator.register_count; ++operation) {
        struct RA_register *reg = &context->allocator.registers[operation];
        reg->external_id = (uint64_t)reg->id + UINT64_C(1);
        reg->external_canonical = reg->external_id;
    }
    for (operation = 0u; operation < context->selected_count; ++operation) {
        unsigned selected_operation = context->schedule.order[operation];
        AP_Selected *selected = &context->selected[selected_operation];
        AP_Operation *source = &context->operations[selected->operation];
        Emitter_Instruction_Info info;
        size_t operand, input = 0u;
        enum RA_status ra_status = RA_OK;
        if (!Emitter_instruction_info(selected->instruction, &info))
            return AP_fail(context, "selected instruction metadata is missing");
        for (operand = 0u; operand < info.operand_count; ++operand) {
            Emitter_Operand_Info expected;
            uint32_t value;
            if (!Emitter_operand_info(selected->instruction, operand, &expected))
                return AP_fail(context, "selected operand metadata is missing");
            if (expected.role == EMITTER_ROLE_OUTPUT) {
                value = source->result + UINT32_C(1);
                ra_status = RA_add_definition(
                    &context->allocator, (uint32_t)operation, value);
            } else {
                const AP_Input *actual = &source->inputs[input++];
                const AP_Value *source_value;
                if (actual->kind != AP_INPUT_VALUE)
                    continue;
                source_value = AP_find_value(context, actual->value);
                if (source_value == NULL)
                    return AP_fail(context, "selected register input has no value");
                value = actual->value + UINT32_C(1);
                ra_status = RA_add_use(
                    &context->allocator, (uint32_t)operation, value);
                /* A parameter already occupies a register when the fragment
                 * starts, even if its first use is later in the schedule. */
                if (ra_status == RA_OK && !source_value->has_definition)
                    ra_status = RA_add_range(
                        &context->allocator, value, 0u,
                        (uint64_t)operation + UINT64_C(1));
            }
            if (ra_status == RA_OK)
                ra_status = RA_constrain_registers(
                    &context->allocator, value,
                    expected.register_min, expected.register_max);
            if (ra_status != RA_OK)
                break;
        }
        if (ra_status != RA_OK)
            return AP_fail(
                context, "cannot construct register intervals: %s",
                RA_status_string(ra_status));
    }
    context->allocator.scheduled_stream_id = context->scheduled_stream_id;
    context->allocator.reg_map_stream_id = context->reg_map_stream_id;
    return AP_write_reg_map(context);

failed:
    if (output != NULL)
        (void)fclose(output);
closed_failed:
    (void)remove("Scheduled.i");
    return AP_fail(context, "cannot write Scheduled.i");
}

static int AP_allocated_register(
        const struct RA_allocator *allocator, uint32_t value,
        uint32_t *physical) {
    const struct RA_interval *interval = RA_find_interval(allocator, value);
    if (interval == NULL || interval->spilled ||
            interval->assigned_register == RA_NO_REGISTER)
        return 0;
    *physical = interval->assigned_register;
    return 1;
}

static int AP_write_emission_operand(
        FILE *output, const struct RA_allocator *allocator,
        const AP_Operation *source, size_t *input,
        const Emitter_Operand_Info *operand) {
    uint32_t physical;
    const AP_Input *value;
    if (operand->role == EMITTER_ROLE_OUTPUT) {
        return source->has_result &&
               AP_allocated_register(
                   allocator, source->result + UINT32_C(1), &physical) &&
               fprintf(
                   output, "REGALLOC_OPERAND_REGISTER(%" PRIu32 ")\n",
                   physical) >= 0;
    }
    if (operand->role != EMITTER_ROLE_INPUT ||
            *input >= source->input_count)
        return 0;
    value = &source->inputs[(*input)++];
    if (value->kind == AP_INPUT_VALUE)
        return AP_allocated_register(
                   allocator, value->value + UINT32_C(1), &physical) &&
               fprintf(
                   output, "REGALLOC_OPERAND_REGISTER(%" PRIu32 ")\n",
                   physical) >= 0;
    if (value->kind == AP_INPUT_IMMEDIATE)
        return fprintf(
            output, "REGALLOC_OPERAND_IMMEDIATE(%" PRId64 ")\n",
            AP_encoding_immediate(source, value, operand)) >= 0;
    return value->kind == AP_INPUT_SYMBOL &&
           fputs("REGALLOC_OPERAND_SYMBOL(", output) != EOF &&
           AP_write_string(output, value->symbol) &&
           fputs(")\n", output) != EOF;
}

static int AP_allocate(AP_Context *context) {
    struct RA_diagnostic diagnostic;
    enum RA_status status;
    FILE *output = NULL;
    size_t position;
    memset(&diagnostic, 0, sizeof diagnostic);
    status = RA_allocate(&context->allocator, &diagnostic);
    if (status != RA_OK)
        return AP_fail(
            context, "register allocation failed: %s: %s",
            RA_status_string(status), diagnostic.message);
    output = fopen("Regallocated.i", "wb");
    if (output == NULL ||
        RA_write(output, &context->allocator, &diagnostic) != RA_OK ||
        fprintf(
            output, "\nREGALLOC_EMISSION_BEGIN(%zu)\n"
                    "REGALLOC_LABEL(0)\n",
            context->selected_count) < 0)
        goto failed;
    for (position = 0u; position < context->selected_count; ++position) {
        unsigned selected_operation = context->schedule.order[position];
        AP_Selected *selected = &context->selected[selected_operation];
        AP_Operation *source =
            &context->operations[selected->operation];
        Emitter_Instruction_Info info;
        size_t operand, input = 0u;
        if (!Emitter_instruction_info(selected->instruction, &info) ||
            fputs("REGALLOC_INSTRUCTION_BEGIN(", output) == EOF ||
            !AP_write_string(output, info.name) ||
            fprintf(output, ", %zu)\n", info.operand_count) < 0)
            goto failed;
        for (operand = 0u; operand < info.operand_count; ++operand) {
            Emitter_Operand_Info operand_info;
            if (!Emitter_operand_info(
                    selected->instruction, operand, &operand_info) ||
                !AP_write_emission_operand(
                    output, &context->allocator, source,
                    &input, &operand_info))
                goto unsupported;
        }
        if (input != source->input_count ||
            fputs("REGALLOC_INSTRUCTION_END()\n", output) == EOF)
            goto failed;
    }
    if (fputs("REGALLOC_EMISSION_END()\n", output) == EOF ||
            fclose(output) != 0)
        goto closed_failed;
    return 1;

unsupported:
    (void)AP_fail(
        context,
        "spills, tied operands, or implicit results require "
        "target-specific lowering");
failed:
    if (output != NULL)
        (void)fclose(output);
closed_failed:
    (void)remove("Regallocated.i");
    if (context->diagnostic[0] == '\0')
        (void)AP_fail(context, "cannot write Regallocated.i");
    return 0;
}

typedef struct AP_RewriteSummary {
    size_t attempted;
    size_t changed;
    size_t iterations;
    size_t applications;
    unsigned saturated;
    ArchonPipelineRewrite engine;
} AP_RewriteSummary;

static const char *AP_archsem_operator(const char *primitive) {
    if (primitive == NULL)
        return NULL;
    if (strcmp(primitive, "add") == 0)
        return "+%";
    if (strcmp(primitive, "sub") == 0)
        return "-%";
    if (strcmp(primitive, "mul") == 0)
        return "*%";
    if (strcmp(primitive, "and") == 0)
        return "&";
    if (strcmp(primitive, "or") == 0)
        return "|";
    if (strcmp(primitive, "xor") == 0)
        return "^";
    if (strcmp(primitive, "shl") == 0)
        return "<<";
    if (strcmp(primitive, "lshr") == 0 ||
            strcmp(primitive, "ashr") == 0)
        return ">>";
    if (strcmp(primitive, "not") == 0)
        return "~";
    return NULL;
}

static uint64_t AP_width_bits(uint64_t bits, unsigned width) {
    if (width != 0u && width < 64u)
        bits &= (UINT64_C(1) << width) - UINT64_C(1);
    return bits;
}

static int AP_run_rewrite_operation(
        AP_Context *context, const AP_Operation *source,
        ArchonPipelineRewrite engine, size_t max_iterations,
        AP_RewriteSummary *summary) {
    const char *semantic_operator = AP_archsem_operator(source->primitive);
    ArchonRewriteGraph graph;
    ArchonGraphsAPI api;
    ArchonGraphsNodeId children[ARCHON_REWRITE_PATTERN_ARITY];
    ArchonGraphsNodeId root, result;
    ArchonRewriteLimits limits;
    ArchonRewriteStats stats;
    ArchonRewriteStatus status;
    size_t input;
    char key[96];
    int length;
    if (semantic_operator == NULL || source->target != NULL ||
            source->unsupported != NULL || source->barrier ||
            source->input_count > ARCHON_REWRITE_PATTERN_ARITY)
        return 1;
    for (input = 0u; input < source->input_count; ++input)
        if (source->inputs[input].kind == AP_INPUT_SYMBOL)
            break;
    if (input != source->input_count)
        return 1;
    status = archon_rewrite_graph_init(&graph);
    if (status != ARCHON_REWRITE_OK)
        return AP_fail(
            context, "cannot initialize rewrite graph: %s",
            archon_rewrite_status_string(status));
    for (input = 0u; input < source->input_count; ++input) {
        const AP_Input *value = &source->inputs[input];
        uint32_t opcode;
        uint32_t flags = ARCHON_GRAPHS_FLAG_PURE;
        uint64_t payload;
        if (value->kind == AP_INPUT_IMMEDIATE) {
            length = snprintf(
                key, sizeof key, "sem:constant:bits:%u",
                value->width != 0u ? value->width : AP_WORD_WIDTH);
            flags |= ARCHON_GRAPHS_FLAG_CONSTANT;
            payload = AP_width_bits(
                (uint64_t)value->immediate,
                value->width != 0u ? value->width : AP_WORD_WIDTH);
        } else {
            length = snprintf(
                key, sizeof key, "source:value:bits:%u",
                value->width != 0u ? value->width : AP_WORD_WIDTH);
            payload = value->value;
        }
        if (length < 0 || (size_t)length >= sizeof key) {
            archon_rewrite_graph_dispose(&graph);
            return AP_fail(context, "rewrite opcode key is too long");
        }
        opcode = archon_rewrite_opcode(key);
        status = archon_rewrite_graph_intern(
            &graph, opcode, payload, flags, NULL, 0u,
            &children[input]);
        if (status != ARCHON_REWRITE_OK) {
            archon_rewrite_graph_dispose(&graph);
            return AP_fail(
                context, "cannot add rewrite operand: %s",
                archon_rewrite_status_string(status));
        }
    }
    length = snprintf(
        key, sizeof key, "sem:%s:bits:%u",
        semantic_operator, source->result_width);
    if (length < 0 || (size_t)length >= sizeof key) {
        archon_rewrite_graph_dispose(&graph);
        return AP_fail(context, "rewrite operation key is too long");
    }
    status = archon_rewrite_graph_intern(
        &graph, archon_rewrite_opcode(key), UINT64_C(0),
        ARCHON_GRAPHS_FLAG_PURE, children, source->input_count, &root);
    if (status != ARCHON_REWRITE_OK) {
        archon_rewrite_graph_dispose(&graph);
        return AP_fail(
            context, "cannot add rewrite operation: %s",
            archon_rewrite_status_string(status));
    }
    api = archon_rewrite_graph_api(&graph);
    limits = archon_rewrite_default_limits();
    limits.max_iterations = max_iterations;
    memset(&stats, 0, sizeof stats);
    status = archon_rewrite_run_generated(
        &api, root,
        engine == ARCHON_PIPELINE_REWRITE_EGRAPH
            ? ARCHON_REWRITE_EQUALITY_SATURATION
            : ARCHON_REWRITE_DIRECTED,
        &limits, &result, &stats);
    if (status != ARCHON_REWRITE_OK) {
        archon_rewrite_graph_dispose(&graph);
        return AP_fail(
            context, "%s rewrite engine failed: %s",
            engine == ARCHON_PIPELINE_REWRITE_EGRAPH
                ? "e-graph" : "conventional",
            archon_rewrite_status_string(status));
    }
    ++summary->attempted;
    summary->changed += result != root;
    summary->iterations += stats.iterations;
    summary->applications += stats.applications;
    summary->saturated &= stats.saturated != 0;
    archon_rewrite_graph_dispose(&graph);
    return 1;
}

static int AP_run_rewrite_engine(
        AP_Context *context, ArchonPipelineRewrite engine,
        const char *annotation_path,
        AP_RewriteSummary *summary) {
    ArchonRewriteLimits default_limits = archon_rewrite_default_limits();
    size_t max_iterations = default_limits.max_iterations;
    size_t count = context->operation_count;
    size_t *order = NULL;
    int64_t *priorities = NULL;
    unsigned char *forbidden = NULL;
    size_t operation;
    int enabled = 1;
    int success = 1;
@ORACLE_REWRITE_LOCALS@
    (void)annotation_path;
    memset(summary, 0, sizeof *summary);
    summary->saturated = 1u;
    if (count > SIZE_MAX / sizeof *order) {
        (void)AP_fail(context, "rewrite operation table is too large");
        success = 0;
        goto rewrite_done;
    }
    order = (size_t *)malloc(
        (count != 0u ? count : 1u) * sizeof *order);
    priorities = (int64_t *)calloc(
        count != 0u ? count : 1u, sizeof *priorities);
    forbidden = (unsigned char *)calloc(
        count != 0u ? count : 1u, sizeof *forbidden);
    if (order == NULL || priorities == NULL || forbidden == NULL) {
        (void)AP_fail(context, "out of memory while preparing rewrites");
        success = 0;
        goto rewrite_done;
    }
@ORACLE_REWRITE_LOAD@
    for (operation = 0u; operation < count; ++operation) {
        order[operation] = operation;
@ORACLE_REWRITE_OPERATION@
    }
    for (operation = 1u; operation < count; ++operation) {
        size_t candidate = order[operation];
        size_t position = operation;
        while (position != 0u &&
                priorities[order[position - 1u]] <
                    priorities[candidate]) {
            order[position] = order[position - 1u];
            --position;
        }
        order[position] = candidate;
    }
    if (enabled)
        for (operation = 0u; operation < count; ++operation) {
            size_t selected = order[operation];
            if (!forbidden[selected] &&
                    !AP_run_rewrite_operation(
                        context, &context->operations[selected],
                        engine, max_iterations, summary)) {
                success = 0;
                goto rewrite_done;
            }
        }
    summary->engine = engine;

rewrite_done:
@ORACLE_REWRITE_DISPOSE@
    free(forbidden);
    free(priorities);
    free(order);
    return success;
}

static int AP_write_rewrite_artifact(
        AP_Context *context, const AP_RewriteSummary *summary) {
    FILE *output = NULL;
    const char *name =
        summary->engine == ARCHON_PIPELINE_REWRITE_EGRAPH
        ? "egraph" : "conventional";
    output = fopen("Rewritten.i", "wb");
    if (output == NULL)
        goto failed;
    if (fputs("ARCHON_REWRITE_IDENTITY(", output) == EOF ||
        !AP_write_string(output, AP_representation) ||
        fprintf(
            output, ", %zu, ", context->operation_count) < 0 ||
        !AP_write_string(output, name) ||
        fputs(", ARCHON_EQUIVALENCE_UNKNOWN)\n", output) == EOF ||
        fputs("ARCHON_REWRITE_ENGINE_STATS(", output) == EOF ||
        !AP_write_string(output, name) ||
        fprintf(
            output, ", %zu, %zu, %zu, %zu, %u)\n",
            summary->attempted, summary->changed, summary->iterations,
            summary->applications, summary->saturated) < 0)
        goto failed;
    if (fclose(output) != 0) {
        output = NULL;
        goto closed_failed;
    }
    output = NULL;
    return 1;

failed:
    if (output != NULL)
        (void)fclose(output);
closed_failed:
    (void)remove("Rewritten.i");
    return AP_fail(context, "cannot write Rewritten.i");
}

static int AP_write_rewrite(
        AP_Context *context, ArchonPipelineRewrite engine) {
    AP_RewriteSummary summary;
@ORACLE_REWRITE_PREPARE@
    /*
     * A configured worker analyzes a safely closed provisional artifact.
     * Its validated sidecar is then consumed before the final rewrite
     * decision.  With no endpoint, a prewritten sidecar is consumed directly.
     */
    if (!AP_run_rewrite_engine(
            context, engine, rewrite_advice_path, &summary) ||
            !AP_write_rewrite_artifact(context, &summary))
        return 0;
@ORACLE_PROCESS_REWRITE@
@ORACLE_REWRITE_ACTIVE_PASS@
    return 1;
}

static void AP_context_init(AP_Context *context) {
    /*
     * Some conservative adapters intentionally reject every operation before
     * constructing the neutral view.  Keep the shared helpers referenced in
     * those generated translation units without compiler-specific attributes.
     */
    (void)AP_value_input;
    (void)AP_add_block;
    (void)AP_add_operation;
    memset(context, 0, sizeof *context);
@ORACLE_ANNOTATION_CONTEXT_INIT@
    AP_adapter_init(context);
}

static void AP_context_dispose(AP_Context *context) {
    size_t operation;
    if (context == NULL)
        return;
@ORACLE_ANNOTATION_CONTEXT_DISPOSE@
    for (operation = 0u; operation < context->operation_count; ++operation)
        free(context->operations[operation].inputs);
    free(context->values);
    free(context->blocks);
    free(context->operations);
    free(context->selected);
    free(context->schedule_operations);
    free(context->dependencies);
    isched_result_destroy(&context->schedule);
    if (context->allocator_initialized)
        RA_destroy(&context->allocator);
    AP_adapter_dispose(context);
    memset(context, 0, sizeof *context);
}

int archon_pipeline_emit_program(
        FILE *output, char *diagnostic, size_t diagnostic_capacity) {
    AP_Context context;
    int success;
    if (diagnostic != NULL && diagnostic_capacity != 0u)
        diagnostic[0] = '\0';
    if (output == NULL) {
        if (diagnostic != NULL && diagnostic_capacity != 0u)
            (void)snprintf(
                diagnostic, diagnostic_capacity,
                "null Program output stream");
        return 1;
    }
    AP_context_init(&context);
    success = AP_adapter_decode(&context) &&
              AP_adapter_lower(&context);
    if (success) {
        context.decoded = 1u;
        AP_assign_stream_ids(&context);
        success = AP_emit_program(&context, output);
    }
    if (!success && diagnostic != NULL && diagnostic_capacity != 0u) {
        (void)snprintf(
            diagnostic, diagnostic_capacity, "%s",
            context.diagnostic[0] != '\0'
                ? context.diagnostic : "Program normalization failed");
        diagnostic[diagnostic_capacity - 1u] = '\0';
    }
    AP_context_dispose(&context);
    return success ? 0 : 1;
}

int archon_pipeline_emit_mir(
        FILE *output, char *diagnostic, size_t diagnostic_capacity) {
    AP_Context context;
    int success;
    if (diagnostic != NULL && diagnostic_capacity != 0u)
        diagnostic[0] = '\0';
    if (output == NULL) {
        if (diagnostic != NULL && diagnostic_capacity != 0u)
            (void)snprintf(
                diagnostic, diagnostic_capacity, "null MIR output stream");
        return 1;
    }
    AP_context_init(&context);
    success = AP_adapter_decode(&context) &&
              AP_adapter_lower(&context);
    if (success) {
        context.decoded = 1u;
        AP_assign_stream_ids(&context);
        success = AP_emit_mir(&context, output);
    }
    if (!success && diagnostic != NULL && diagnostic_capacity != 0u) {
        (void)snprintf(
            diagnostic, diagnostic_capacity, "%s",
            context.diagnostic[0] != '\0'
                ? context.diagnostic : "MIR lowering failed");
        diagnostic[diagnostic_capacity - 1u] = '\0';
    }
    AP_context_dispose(&context);
    return success ? 0 : 1;
}

void archon_pipeline_options_init(ArchonPipelineOptions *options) {
    if (options != NULL) {
        memset(options, 0, sizeof *options);
        options->stop_after = ARCHON_PIPELINE_EMIT;
        options->rewrite = ARCHON_PIPELINE_REWRITE_CONVENTIONAL;
    }
}

void archon_pipeline_result_init(ArchonPipelineResult *result) {
    if (result != NULL) {
        memset(result, 0, sizeof *result);
        result->status = ARCHON_PIPELINE_STATUS_OK;
        result->emitter_status = EMITTER_STATUS_OK;
    }
}

ArchonPipelineStatus archon_pipeline_run_with_result(
        const ArchonPipelineOptions *options,
        ArchonPipelineResult *result) {
    AP_Context context;
    ArchonPipelineStage stage;
    FILE *diagnostics;
    ArchonPipelineResult local_result;
    int success = 1;
    if (result == NULL)
        result = &local_result;
    archon_pipeline_result_init(result);
    if (options == NULL ||
            options->stop_after < ARCHON_PIPELINE_DECODE ||
            options->stop_after > ARCHON_PIPELINE_EMIT ||
            (options->rewrite != ARCHON_PIPELINE_REWRITE_CONVENTIONAL &&
             options->rewrite != ARCHON_PIPELINE_REWRITE_EGRAPH)) {
        result->status = ARCHON_PIPELINE_STATUS_INVALID_ARGUMENT;
        (void)snprintf(
            result->diagnostic, sizeof result->diagnostic,
            "invalid pipeline options");
        if (options != NULL && options->diagnostics != NULL)
            (void)fprintf(
                options->diagnostics, "pipeline: options: %s\n",
                result->diagnostic);
        return result->status;
    }
    diagnostics = options->diagnostics;
    AP_context_init(&context);
@ORACLE_CONTEXT_INIT@
    stage = ARCHON_PIPELINE_DECODE;
    if (!AP_adapter_decode(&context) ||
            !AP_adapter_lower(&context)) {
        success = 0;
        goto done;
    }
    context.decoded = 1u;
    AP_assign_stream_ids(&context);
    if (options->stop_after == stage)
        goto done;
    stage = ARCHON_PIPELINE_REWRITE;
    if (!AP_write_rewrite(&context, options->rewrite)) {
        success = 0;
        goto done;
    }
    if (options->stop_after == stage)
        goto done;
    stage = ARCHON_PIPELINE_SCHEDULE;
    if (!AP_write_mir(&context)) {
        success = 0;
        goto done;
    }
    if (options->stop_after == stage)
        goto done;
    stage = ARCHON_PIPELINE_ISEL;
    if (!AP_select(&context)) {
        success = 0;
        goto done;
    }
    if (options->stop_after == stage)
        goto done;
    stage = ARCHON_PIPELINE_ISCHED;
    if (!AP_schedule(&context)) {
        success = 0;
        goto done;
    }
    if (options->stop_after == stage)
        goto done;
    stage = ARCHON_PIPELINE_REGALLOC;
@ORACLE_PROCESS_REGMAP@
    if (!AP_allocate(&context)) {
        success = 0;
        goto done;
    }
    if (options->stop_after == stage)
        goto done;
    stage = ARCHON_PIPELINE_EMIT;
    if (options->emitter == NULL) {
        (void)AP_fail(&context, "missing emitter options");
        success = 0;
    } else {
        Emitter_Diagnostic diagnostic;
@ORACLE_EMITTER_LOCAL@
        if (Emitter_emit(
                @ORACLE_EMITTER_ARGUMENT@,
                &diagnostic) != EMITTER_STATUS_OK) {
            result->emitter_status = diagnostic.status;
            result->emitter_artifact_line = diagnostic.artifact_line;
            (void)AP_fail(
                &context, "assembly emission failed: %s",
                diagnostic.message);
            success = 0;
        }
    }

done:
    result->stage = stage;
    result->stage_valid = 1u;
    result->status = success
        ? ARCHON_PIPELINE_STATUS_OK
        : ARCHON_PIPELINE_STATUS_STAGE_FAILED;
    if (!success) {
        (void)snprintf(
            result->diagnostic, sizeof result->diagnostic, "%s",
            context.diagnostic[0] != '\0'
                ? context.diagnostic : "unknown failure");
        result->diagnostic[sizeof result->diagnostic - 1u] = '\0';
    }
    if (!success && diagnostics != NULL)
        (void)fprintf(
            diagnostics, "pipeline: %s stage: %s\n",
            stage == ARCHON_PIPELINE_DECODE ? "decode" :
            stage == ARCHON_PIPELINE_REWRITE ? "rewrite" :
            stage == ARCHON_PIPELINE_SCHEDULE ? "schedule" :
            stage == ARCHON_PIPELINE_ISEL ? "isel" :
            stage == ARCHON_PIPELINE_ISCHED ? "isched" :
            stage == ARCHON_PIPELINE_REGALLOC ? "regalloc" : "emit",
            result->diagnostic);
    AP_context_dispose(&context);
    return result->status;
}

int archon_pipeline_run(const ArchonPipelineOptions *options) {
    return archon_pipeline_run_with_result(options, NULL) ==
        ARCHON_PIPELINE_STATUS_OK ? 0 : 1;
}
'''


# Adapter snippets are inserted before AP_Context is complete.  They may declare
# their program type and function prototypes there; function bodies follow the
# common helper definitions through the tail snippets below.
_ADAPTER_PREFIX = r'''
typedef @PROGRAM_TYPE@ AP_AdapterProgram;
static void AP_adapter_init(AP_Context *context);
static void AP_adapter_dispose(AP_Context *context);
static int AP_adapter_decode(AP_Context *context);
static int AP_adapter_lower(AP_Context *context);
@ADAPTER_BODY@
'''


_ADAPTER_ANF = _ADAPTER_PREFIX.replace(
    "@PROGRAM_TYPE@", "struct archon_ir_program"
) + r'''
static void AP_adapter_init(AP_Context *context) {
    archon_ir_program_init(&context->program);
}
static void AP_adapter_dispose(AP_Context *context) {
    if (context->decoded)
        archon_ir_program_dispose(&context->program);
}
static int AP_adapter_decode(AP_Context *context) {
    char message[256];
    enum archon_ir_status status = archon_ir_load_embedded(
        &context->program, message, sizeof message);
    if (status != ARCHON_IR_OK)
        return AP_fail(
            context, "ANF decode failed: %s",
            message[0] != '\0'
                ? message : archon_ir_status_name(status));
    context->decoded = 1u;
    status = archon_ir_verify(
        &context->program, message, sizeof message);
    return status == ARCHON_IR_OK ||
        AP_fail(context, "invalid ANF: %s", message);
}
static unsigned AP_anf_signed(struct archon_ir_type type) {
    return type.kind == ARCHON_IR_T_I8 ||
           type.kind == ARCHON_IR_T_I16 ||
           type.kind == ARCHON_IR_T_I32 ||
           type.kind == ARCHON_IR_T_I64;
}
static int64_t AP_anf_integer(const struct archon_ir_atom *atom) {
    uint64_t bits = atom->as.integer;
    unsigned width = atom->type.width;
    uint64_t mask, sign;
    if (!AP_anf_signed(atom->type) || width == 0u || width >= 64u)
        return (int64_t)bits;
    mask = (UINT64_C(1) << width) - UINT64_C(1);
    sign = UINT64_C(1) << (width - 1u);
    bits &= mask;
    if ((bits & sign) != 0u)
        return -(int64_t)(((~bits) & mask) + UINT64_C(1));
    return (int64_t)bits;
}
static int AP_adapter_lower(AP_Context *context) {
    const struct archon_ir_function *function;
    size_t value_index, block_index, operation_index, input_index;
    if (context->program.function_count != 1u)
        return AP_fail(
            context,
            "ANF ABI lowering requires exactly one source function");
    function = &context->program.functions[0];
    for (block_index = 0u;
         block_index < function->block_count; ++block_index)
        if (!AP_add_block(
                context, (uint32_t)block_index,
                function->blocks[block_index].name))
            return 0;
    for (value_index = 0u; value_index < function->value_count;
         ++value_index) {
        const struct archon_ir_value *value =
            &function->values[value_index];
        if (value->is_parameter &&
            !AP_add_value(
                context, (uint32_t)value_index, value->type.width,
                AP_anf_signed(value->type), 0u, 0u))
            return 0;
    }
    for (block_index = 0u;
         block_index < function->block_count; ++block_index) {
        const struct archon_ir_block *block =
            &function->blocks[block_index];
        for (operation_index = 0u;
             operation_index < block->operation_count;
             ++operation_index) {
            const struct archon_ir_operation *operation =
                &block->operations[operation_index];
            AP_Input inputs[64];
            const char *unsupported = NULL;
            if (operation->kind == ARCHON_IR_OP_RETURN)
                continue;
            if (operation->kind == ARCHON_IR_OP_CALL)
                unsupported = "ANF call requires validated ABI lowering";
            else if (operation->kind == ARCHON_IR_OP_BRANCH ||
                     operation->kind == ARCHON_IR_OP_JUMP)
                unsupported =
                    "ANF control flow requires validated CFG lowering";
            else if (operation->effects != ARCHON_IR_EFFECT_NONE)
                unsupported =
                    "ANF effect requires validated memory/effect lowering";
            if (unsupported != NULL) {
                if (!AP_add_operation(
                        context, (uint32_t)block_index, NULL, NULL,
                        unsupported, 0u, UINT32_MAX, 0u,
                        NULL, 0u, 1u))
                    return 0;
                continue;
            }
            if (operation->argument_count >
                    sizeof inputs / sizeof inputs[0])
                return AP_fail(context, "ANF operation is too wide");
            for (input_index = 0u;
                 input_index < operation->argument_count; ++input_index) {
                const struct archon_ir_atom *atom =
                    &operation->arguments[input_index];
                memset(&inputs[input_index], 0, sizeof inputs[input_index]);
                if (atom->kind == ARCHON_IR_ATOM_VALUE) {
                    if (!AP_value_input(
                            context, atom->as.value,
                            &inputs[input_index]))
                        return 0;
                } else if (atom->kind == ARCHON_IR_ATOM_INTEGER) {
                    inputs[input_index].kind = AP_INPUT_IMMEDIATE;
                    inputs[input_index].immediate = AP_anf_integer(atom);
                    inputs[input_index].width = atom->type.width;
                    inputs[input_index].is_signed =
                        AP_anf_signed(atom->type);
                } else if (atom->kind == ARCHON_IR_ATOM_BOOLEAN) {
                    inputs[input_index].kind = AP_INPUT_IMMEDIATE;
                    inputs[input_index].immediate =
                        atom->as.boolean != 0;
                    inputs[input_index].width = 1u;
                } else {
                    inputs[input_index].kind = AP_INPUT_SYMBOL;
                    inputs[input_index].symbol = atom->as.symbol;
                }
            }
            if (!AP_add_operation(
                    context, (uint32_t)block_index,
                    archon_ir_primitive_name(operation->primitive),
                    NULL, NULL,
                    operation->result != ARCHON_IR_NO_VALUE,
                    operation->result, operation->result_type.width,
                    inputs, operation->argument_count, 0u))
                return 0;
        }
    }
    return 1;
}
'''


_ADAPTER_MEMORY_SSA = _ADAPTER_PREFIX.replace(
    "@PROGRAM_TYPE@", "struct IR_program"
) + r'''
static void AP_adapter_init(AP_Context *context) {
    memset(&context->program, 0, sizeof context->program);
}
static void AP_adapter_dispose(AP_Context *context) {
    if (context->decoded)
        IR_program_destroy(&context->program);
}
static int AP_adapter_decode(AP_Context *context) {
    struct IR_diagnostic diagnostic;
    enum IR_status status;
    memset(&diagnostic, 0, sizeof diagnostic);
    status = IR_load_program(&context->program, &diagnostic);
    if (status != IR_OK)
        return AP_fail(context, "Memory SSA decode failed: %s",
                       diagnostic.message);
    context->decoded = 1u;
    status = IR_verify(&context->program, &diagnostic);
    return status == IR_OK ||
        AP_fail(context, "invalid Memory SSA: %s", diagnostic.message);
}
static int AP_adapter_lower(AP_Context *context) {
    size_t block_index, node_index;
    for (block_index = 0u;
         block_index < context->program.block_count; ++block_index) {
        struct IR_block *block = &context->program.blocks[block_index];
        if (!AP_add_block(context, block->id, block->name))
            return 0;
        for (node_index = 0u; node_index < block->node_count; ++node_index) {
            struct IR_node *node =
                &context->program.nodes[block->nodes[node_index]];
            if (node->kind == IR_NODE_PARAMETER) {
                if (!AP_add_value(
                        context, node->result, node->type.width,
                        node->type.kind == IR_TYPE_SINT, 0u, 0u))
                    return 0;
            } else if (node->kind == IR_NODE_MEMORY_PARAMETER) {
                continue;
            } else if (node->kind == IR_NODE_CONSTANT) {
                if (!AP_add_value(
                        context, node->result, node->type.width,
                        node->type.kind == IR_TYPE_SINT, 1u,
                        node->immediate))
                    return 0;
            } else if (node->kind == IR_NODE_TARGET) {
                const struct IR_operation_metadata *metadata =
                    IR_operation_metadata(node->operation);
                const struct IR_architecture_metadata *architecture;
                AP_Input inputs[64];
                size_t input;
                if (metadata == NULL ||
                        node->operand_count > sizeof inputs / sizeof inputs[0])
                    return AP_fail(
                        context, "invalid Memory SSA target operation");
                architecture = IR_architecture(metadata->architecture);
                if (architecture == NULL)
                    return AP_fail(context, "missing Memory SSA target architecture");
                if (strcmp(architecture->name, Emitter_target_name()) != 0) {
                    if (!AP_add_operation(
                            context, block->id, NULL, NULL,
                            "explicit Memory SSA instruction belongs to a different target",
                            node->result != IR_NO_VALUE, node->result,
                            node->type.width, NULL, 0u, 1u))
                        return 0;
                    continue;
                }
                for (input = 0u; input < node->operand_count; ++input)
                    if (!AP_value_input(
                            context, node->operands[input], &inputs[input]))
                        return 0;
                if (!AP_add_operation(
                        context, block->id, NULL, metadata->name, NULL,
                        node->result != IR_NO_VALUE, node->result,
                        node->type.width, inputs, node->operand_count,
                        metadata->requires_sequence))
                    return 0;
            } else if (node->kind == IR_NODE_RETURN) {
                continue;
            } else {
                if (!AP_add_operation(
                        context, block->id, NULL, NULL,
                        "Memory SSA operation has no validated target lowering",
                        0u, UINT32_MAX, 0u, NULL, 0u, 1u))
                    return 0;
            }
        }
    }
    return 1;
}
'''


_ADAPTER_TYPED_TAC = _ADAPTER_PREFIX.replace(
    "@PROGRAM_TYPE@", "IR_Program"
) + r'''
static void AP_adapter_init(AP_Context *context) {
    memset(&context->program, 0, sizeof context->program);
}
static void AP_adapter_dispose(AP_Context *context) {
    if (context->decoded)
        ir_program_destroy(&context->program);
}
static int AP_adapter_decode(AP_Context *context) {
    IR_Diagnostic diagnostic;
    if (!ir_load_program_i(&context->program, &diagnostic))
        return AP_fail(context, "typed TAC decode failed: %s",
                       diagnostic.message);
    context->decoded = 1u;
    return ir_verify(&context->program, &diagnostic) ||
        AP_fail(context, "invalid typed TAC: %s", diagnostic.message);
}
static const char *AP_typed_primitive(enum IR_OpKind kind) {
    switch (kind) {
    case IR_OP_COPY: return "copy";
    case IR_OP_ADD: return "add";
    case IR_OP_SUB: return "sub";
    case IR_OP_MUL: return "mul";
    case IR_OP_AND: return "and";
    case IR_OP_OR: return "or";
    case IR_OP_XOR: return "xor";
    case IR_OP_SHL: return "shl";
    case IR_OP_LSHR: return "lshr";
    case IR_OP_ASHR: return "ashr";
    default: return NULL;
    }
}
static int AP_adapter_lower(AP_Context *context) {
    size_t block_index, operation_index;
    for (block_index = 0u;
         block_index < context->program.block_count; ++block_index) {
        IR_BasicBlock *block = &context->program.blocks[block_index];
        if (!AP_add_block(
                context, (uint32_t)block_index,
                context->program.strings + block->name_offset))
            return 0;
    }
    for (operation_index = 0u;
         operation_index < context->program.operation_count;
         ++operation_index) {
        IR_Operation *operation =
            &context->program.operations[operation_index];
        if (operation->kind == IR_OP_PARAMETER) {
            if (!AP_add_value(
                    context, operation->result, operation->type.width,
                    operation->type.kind == IR_TYPE_SINT, 0u, 0u))
                return 0;
        } else if (operation->kind == IR_OP_EFFECT_START) {
            continue;
        } else if (operation->kind == IR_OP_CONSTANT) {
            if (!AP_add_value(
                    context, operation->result, operation->type.width,
                    operation->type.kind == IR_TYPE_SINT, 1u,
                    operation->immediate))
                return 0;
        } else if (AP_typed_primitive(operation->kind) != NULL) {
            AP_Input inputs[64];
            size_t input;
            if (operation->input_count >
                    sizeof inputs / sizeof inputs[0])
                return AP_fail(context, "typed TAC operation is too wide");
            for (input = 0u; input < operation->input_count; ++input)
                if (!AP_value_input(
                        context,
                        context->program.inputs[
                            operation->first_input + input].temp,
                        &inputs[input]))
                    return 0;
            if (!AP_add_operation(
                    context, operation->block,
                    AP_typed_primitive(operation->kind), NULL, NULL,
                    operation->result != IR_TEMP_NONE, operation->result,
                    operation->type.width, inputs,
                    operation->input_count, 0u))
                return 0;
        } else if (operation->kind == IR_OP_RETURN) {
            continue;
        } else {
            if (!AP_add_operation(
                    context, operation->block, NULL, NULL,
                    "typed TAC operation has no validated target lowering",
                    0u, UINT32_MAX, 0u, NULL, 0u, 1u))
                return 0;
        }
    }
    return 1;
}
'''


_ADAPTER_UNTYPED_TAC = _ADAPTER_PREFIX.replace(
    "@PROGRAM_TYPE@", "struct IR_program"
) + r'''
static void AP_adapter_init(AP_Context *context) {
    memset(&context->program, 0, sizeof context->program);
}
static void AP_adapter_dispose(AP_Context *context) {
    if (context->decoded)
        IR_program_destroy(&context->program);
}
static int AP_adapter_decode(AP_Context *context) {
    struct IR_diagnostic diagnostic;
    enum IR_status status = IR_load_program(&context->program, &diagnostic);
    if (status != IR_OK)
        return AP_fail(context, "untyped TAC decode failed: %s",
                       diagnostic.message);
    context->decoded = 1u;
    status = IR_verify(&context->program, &diagnostic);
    return status == IR_OK ||
        AP_fail(context, "invalid untyped TAC: %s", diagnostic.message);
}
static const char *AP_untyped_operator(enum IR_operator kind) {
    switch (kind) {
    case IR_OPERATOR_NEG: return "neg";
    case IR_OPERATOR_NOT: return "not";
    case IR_OPERATOR_ADD: return "add";
    case IR_OPERATOR_SUB: return "sub";
    case IR_OPERATOR_MUL: return "mul";
    case IR_OPERATOR_AND: return "and";
    case IR_OPERATOR_OR: return "or";
    case IR_OPERATOR_XOR: return "xor";
    case IR_OPERATOR_SHL: return "shl";
    case IR_OPERATOR_SHR: return "lshr";
    default: return NULL;
    }
}
static int AP_adapter_lower(AP_Context *context) {
    size_t index;
    uint32_t block = 0u;
    if (!AP_add_block(context, block, "entry"))
        return 0;
    for (index = 0u; index < context->program.instruction_count; ++index) {
        struct IR_instruction *instruction =
            &context->program.instructions[index];
        if (instruction->opcode == IR_OP_LABEL) {
            if (context->operation_count != 0u)
                return AP_fail(
                    context,
                    "untyped TAC multi-block lowering is unavailable");
        } else if (instruction->opcode == IR_OP_PARAMETER) {
            if (!AP_add_value(
                    context, instruction->result, AP_WORD_WIDTH,
                    0u, 0u, 0u))
                return 0;
        } else if (instruction->opcode == IR_OP_CONSTANT) {
            if (!AP_add_value(
                    context, instruction->result, AP_WORD_WIDTH,
                    1u, 1u, (uint64_t)instruction->immediate))
                return 0;
        } else if (instruction->opcode == IR_OP_COPY ||
                   instruction->opcode == IR_OP_UNARY ||
                   instruction->opcode == IR_OP_BINARY) {
            AP_Input inputs[2];
            const char *primitive = instruction->opcode == IR_OP_COPY
                ? "copy" : AP_untyped_operator(instruction->operator_kind);
            size_t count = instruction->opcode == IR_OP_BINARY ? 2u : 1u;
            if (primitive == NULL ||
                !AP_value_input(context, instruction->first, &inputs[0]) ||
                (count == 2u &&
                 !AP_value_input(
                     context, instruction->second, &inputs[1])) ||
                !AP_add_operation(
                    context, block, primitive, NULL, NULL, 1u,
                    instruction->result, AP_WORD_WIDTH,
                    inputs, count, 0u))
                return 0;
        } else if (instruction->opcode == IR_OP_RETURN) {
            continue;
        } else {
            if (!AP_add_operation(
                    context, block, NULL, NULL,
                    "untyped TAC operation has no validated target lowering",
                    0u, UINT32_MAX, 0u, NULL, 0u, 1u))
                return 0;
        }
    }
    return 1;
}
'''


_ADAPTER_GATED = _ADAPTER_PREFIX.replace(
    "@PROGRAM_TYPE@", "struct archon_ir_program"
) + r'''
static void AP_adapter_init(AP_Context *context) {
    archon_ir_program_init(&context->program);
}
static void AP_adapter_dispose(AP_Context *context) {
    archon_ir_program_dispose(&context->program);
}
static int AP_adapter_decode(AP_Context *context) {
    enum archon_ir_status status =
        archon_ir_load_program_i(&context->program);
    char message[256];
    if (status != ARCHON_IR_OK)
        return AP_fail(context, "Gated SSA decode failed: %s",
                       archon_ir_program_error(&context->program));
    context->decoded = 1u;
    status = archon_ir_verify(
        &context->program, message, sizeof message);
    return status == ARCHON_IR_OK ||
        AP_fail(context, "invalid Gated SSA: %s", message);
}
static const char *AP_gated_target(enum archon_ir_opcode opcode) {
    const char *display = archon_ir_opcode_name(opcode);
    const char *target = Emitter_target_name();
    size_t length = strlen(target);
    if (display == NULL)
        return NULL;
    return strncmp(display, target, length) == 0 && display[length] == '.' &&
        display[length + 1u] != '\0' ? display + length + 1u : NULL;
}
static int AP_adapter_lower(AP_Context *context) {
    size_t block_index, node_index;
    for (block_index = 0u;
         block_index < context->program.block_count; ++block_index)
        if (!AP_add_block(
                context, context->program.blocks[block_index].id,
                context->program.blocks[block_index].name))
            return 0;
    for (node_index = 0u;
         node_index < context->program.node_count; ++node_index) {
        struct archon_ir_node *node = &context->program.nodes[node_index];
        if (node->opcode == ARCHON_IR_OP_PARAMETER) {
            if (!AP_add_value(
                    context, node->id, node->type.width,
                    node->type.kind == ARCHON_IR_TYPE_SINT, 0u, 0u))
                return 0;
        } else if (node->opcode == ARCHON_IR_OP_CONSTANT) {
            if (!AP_add_value(
                    context, node->id, node->type.width,
                    node->type.kind == ARCHON_IR_TYPE_SINT, 1u,
                    node->payload))
                return 0;
        } else if ((unsigned)node->opcode >=
                   (unsigned)ARCHON_IR_OP_TARGET_BASE) {
            AP_Input inputs[64];
            size_t input, count = 0u;
            const char *target = AP_gated_target(node->opcode);
            if (target == NULL) {
                if (!AP_add_operation(
                        context, node->block, NULL, NULL,
                        "explicit Gated SSA instruction belongs to a different target",
                        node->type.kind != ARCHON_IR_TYPE_EFFECT,
                        node->id, node->type.width, NULL, 0u, 1u))
                    return 0;
                continue;
            }
            if (node->input_count >
                        sizeof inputs / sizeof inputs[0])
                return AP_fail(context, "invalid Gated SSA target node");
            /* Verification checked the leading token; it orders instructions,
             * not an encoded operand. Linked scheduling preserves source order. */
            for (input = node->type.kind == ARCHON_IR_TYPE_EFFECT ? 1u : 0u;
                 input < node->input_count; ++input)
                if (!AP_value_input(
                        context,
                        context->program.inputs[
                            node->first_input + input].value,
                        &inputs[count++]))
                    return 0;
            if (!AP_add_operation(
                    context, node->block, NULL, target, NULL,
                    node->type.kind != ARCHON_IR_TYPE_EFFECT,
                    node->id, node->type.width, inputs,
                    count,
                    node->type.kind == ARCHON_IR_TYPE_EFFECT))
                return 0;
        } else if (node->opcode == ARCHON_IR_OP_EFFECT_START) {
            continue;
        } else {
            if (!AP_add_operation(
                    context, node->block, NULL, NULL,
                    "Gated SSA node has no explicit semantic opcode",
                    0u, UINT32_MAX, 0u, NULL, 0u, 1u))
                return 0;
        }
    }
    return 1;
}
'''


_ADAPTER_SEA = _ADAPTER_PREFIX.replace(
    "@PROGRAM_TYPE@", "ArchonIRGraph"
) + r'''
typedef struct AP_SeaTarget {
    uint32_t opcode;
    const char *instruction;
} AP_SeaTarget;
static const AP_SeaTarget AP_sea_targets[] = {
@SEA_TARGET_ROWS@
};
static void AP_adapter_init(AP_Context *context) {
    memset(&context->program, 0, sizeof context->program);
}
static void AP_adapter_dispose(AP_Context *context) {
    if (context->decoded)
        archon_ir_graph_destroy(&context->program);
}
static int AP_adapter_decode(AP_Context *context) {
    ArchonIRNodeId root;
    ArchonIRStatus status =
        archon_ir_graph_init(&context->program, NULL);
    if (status != ARCHON_IR_OK)
        return AP_fail(context, "Sea-of-Nodes initialization failed: %s",
                       archon_ir_status_string(status));
    context->decoded = 1u;
    status = archon_ir_load_program_i(&context->program, &root);
    if (status != ARCHON_IR_OK)
        return AP_fail(context, "Sea-of-Nodes decode failed: %s",
                       archon_ir_status_string(status));
    status = archon_ir_verify(&context->program, root);
    return status == ARCHON_IR_OK ||
        AP_fail(context, "invalid Sea-of-Nodes graph: %s",
                archon_ir_status_string(status));
}
static const char *AP_sea_target(uint32_t opcode) {
    size_t index;
    for (index = 0u;
         index < sizeof AP_sea_targets / sizeof AP_sea_targets[0];
         ++index)
        if (AP_sea_targets[index].instruction != NULL &&
                AP_sea_targets[index].opcode == opcode)
            return AP_sea_targets[index].instruction;
    return NULL;
}
static int AP_adapter_lower(AP_Context *context) {
    size_t node_index;
    if (!AP_add_block(context, 0u, "sea-region"))
        return 0;
    for (node_index = 0u;
         node_index < context->program.node_count; ++node_index) {
        ArchonIRNode *node = &context->program.nodes[node_index];
        const char *target = AP_sea_target(node->opcode);
        if (node->kind == ARCHON_IR_NODE_CONSTANT) {
            if (!AP_add_value(
                    context, node->id, AP_WORD_WIDTH, 0u, 1u,
                    node->payload))
                return 0;
        } else if (target != NULL) {
            AP_Input inputs[64];
            size_t input, count = 0u;
            if (node->input_count >
                    sizeof inputs / sizeof inputs[0])
                return AP_fail(context, "Sea-of-Nodes node is too wide");
            for (input = 0u; input < node->input_count; ++input) {
                ArchonIRInput *edge =
                    &context->program.inputs[node->first_input + input];
                if (edge->kind != ARCHON_IR_INPUT_DATA)
                    continue;
                if (!AP_value_input(context, edge->source, &inputs[count++]))
                    return 0;
            }
            if (!AP_add_operation(
                    context, 0u, NULL, target, NULL,
                    (node->flags & ARCHON_IR_FLAG_PURE) != 0u,
                    node->id, AP_WORD_WIDTH, inputs, count,
                    (node->flags & ARCHON_IR_FLAG_PURE) == 0u))
                return 0;
        } else if (node->kind == ARCHON_IR_NODE_START ||
                   node->kind == ARCHON_IR_NODE_END ||
                   node->kind == ARCHON_IR_NODE_REGION ||
                   node->kind == ARCHON_IR_NODE_MERGE) {
            continue;
        } else {
            if (!AP_add_operation(
                    context, 0u, NULL, NULL,
                    "Sea-of-Nodes node has no validated target semantics",
                    0u, UINT32_MAX, 0u, NULL, 0u, 1u))
                return 0;
        }
    }
    return 1;
}
'''


_ADAPTER_CPS = _ADAPTER_PREFIX.replace(
    "@PROGRAM_TYPE@", "struct ArchonCPSProgram"
) + r'''
static void AP_adapter_init(AP_Context *context) {
    archon_cps_program_init(&context->program);
}
static void AP_adapter_dispose(AP_Context *context) {
    archon_cps_program_dispose(&context->program);
}
static int AP_adapter_decode(AP_Context *context) {
    char message[256];
    enum ArchonCPSStatus status =
        archon_cps_decode_program(
            &context->program, message, sizeof message);
    if (status != ARCHON_CPS_OK)
        return AP_fail(context, "CPS decode failed: %s",
                       message[0] != '\0'
                           ? message : archon_cps_status_string(status));
    context->decoded = 1u;
    status = archon_cps_verify(
        &context->program, message, sizeof message);
    return status == ARCHON_CPS_OK ||
        AP_fail(context, "invalid CPS: %s", message);
}
static int AP_adapter_lower(AP_Context *context) {
    size_t block_index, value_index, node_index;
    for (block_index = 0u;
         block_index < context->program.block_count; ++block_index)
        if (!AP_add_block(
                context, context->program.blocks[block_index].id, "cps"))
            return 0;
    for (value_index = 0u;
         value_index < context->program.value_count; ++value_index) {
        struct ArchonCPSValue *value =
            &context->program.values[value_index];
        struct ArchonCPSType *type;
        if (!(value->flags & ARCHON_CPS_VALUE_PARAMETER))
            continue;
        type = &context->program.types[value->type - 1u];
        if (type->kind == ARCHON_CPS_TYPE_EFFECT ||
                type->kind == ARCHON_CPS_TYPE_CONTROL ||
                type->kind == ARCHON_CPS_TYPE_CONTINUATION)
            continue;
        if (!AP_add_value(
                context, value->id, type->width,
                type->kind == ARCHON_CPS_TYPE_SIGNED, 0u, 0u))
            return 0;
    }
    for (node_index = 0u;
         node_index < context->program.node_count; ++node_index) {
        struct ArchonCPSNode *node = &context->program.nodes[node_index];
        if (node->opcode == ARCHON_CPS_OP_CONSTANT) {
            struct ArchonCPSValue *value =
                &context->program.values[node->result - 1u];
            struct ArchonCPSType *type =
                &context->program.types[value->type - 1u];
            if (!AP_add_value(
                    context, value->id, type->width,
                    type->kind == ARCHON_CPS_TYPE_SIGNED, 1u,
                    node->immediate))
                return 0;
        } else if (node->opcode == ARCHON_CPS_OP_RETURN ||
                   node->opcode == ARCHON_CPS_OP_JUMP ||
                   node->opcode == ARCHON_CPS_OP_BRANCH ||
                   node->opcode == ARCHON_CPS_OP_UNREACHABLE) {
            continue;
        } else {
            if (!AP_add_operation(
                    context, node->block, NULL, NULL,
                    "CPS node carries no source-operation equivalence fact",
                    0u, UINT32_MAX, 0u, NULL, 0u, 1u))
                return 0;
        }
    }
    return 1;
}
'''


_ADAPTER_SUPERCOMBINATORS = _ADAPTER_PREFIX.replace(
    "@PROGRAM_TYPE@", "IR_Program"
) + r'''
static void AP_adapter_init(AP_Context *context) {
    IR_program_init(&context->program);
}
static void AP_adapter_dispose(AP_Context *context) {
    IR_program_clear(&context->program);
}
static int AP_adapter_decode(AP_Context *context) {
    IR_Status status = IR_decode_program(&context->program);
    if (status != IR_STATUS_OK)
        return AP_fail(
            context, "supercombinator decode failed: %s",
            IR_program_diagnostic(&context->program));
    context->decoded = 1u;
    status = IR_verify_program(&context->program);
    return status == IR_STATUS_OK ||
        AP_fail(
            context, "invalid supercombinator program: %s",
            IR_program_diagnostic(&context->program));
}
static int AP_adapter_lower(AP_Context *context) {
    size_t index;
    static const char prefix[] = "target:";
    if (!AP_add_block(context, 0u, "supercombinator"))
        return 0;
    for (index = 0u; index < context->program.count; ++index) {
        IR_Node *node = &context->program.nodes[index];
        if (node->kind == IR_KIND_LITERAL) {
            if (!AP_add_value(
                    context, node->id, AP_WORD_WIDTH, 1u, 1u,
                    (uint64_t)node->as.literal.value))
                return 0;
        } else if (node->kind == IR_KIND_INSTRUCTION) {
            const char *opcode = node->as.instruction.opcode;
            const char *target = NULL;
            AP_Input inputs[64];
            Emitter_Instruction_Info instruction_info;
            size_t instruction_index;
            unsigned has_result = 0u;
            size_t input;
            if (strncmp(opcode, prefix, sizeof prefix - 1u) == 0)
                target = opcode + sizeof prefix - 1u;
            if (target == NULL || *target == '\0') {
                if (!AP_add_operation(
                        context, 0u, NULL, NULL,
                        "supercombinator instruction is not an explicit "
                        "target:<instruction> reference",
                        0u, UINT32_MAX, 0u, NULL, 0u, 1u))
                    return 0;
                continue;
            }
            if (node->as.instruction.operands.count >
                    sizeof inputs / sizeof inputs[0])
                return AP_fail(
                    context, "supercombinator instruction is too wide");
            for (input = 0u;
                  input < node->as.instruction.operands.count; ++input)
                if (!AP_value_input(
                        context,
                        node->as.instruction.operands.items[input],
                        &inputs[input]))
                    return 0;
            /*
             * Derive result shape only from the selected target's declared
             * operand directions.  This supports explicit register-machine
             * instructions without assigning an invented result to stack or
             * registerless instructions.
             */
            if (!Emitter_lookup_instruction(target, &instruction_index) ||
                    !Emitter_instruction_info(
                        instruction_index, &instruction_info))
                return AP_fail(
                    context,
                    "supercombinator names unknown explicit target %s",
                    target);
            for (input = 0u;
                 input < instruction_info.operand_count; ++input) {
                Emitter_Operand_Info operand_info;
                if (!Emitter_operand_info(
                        instruction_index, input, &operand_info))
                    return AP_fail(
                        context,
                        "supercombinator target operand metadata is missing");
                if (operand_info.role == EMITTER_ROLE_OUTPUT)
                    ++has_result;
                else if (operand_info.role == EMITTER_ROLE_INOUT)
                    return AP_fail(
                        context,
                        "supercombinator target uses tied operands");
            }
            if (has_result > 1u)
                return AP_fail(
                    context,
                    "supercombinator target has multiple results");
            if (!AP_add_operation(
                    context, 0u, NULL, target, NULL, has_result,
                    has_result ? node->id : UINT32_MAX,
                    has_result ? AP_WORD_WIDTH : 0u, inputs,
                    node->as.instruction.operands.count, 1u))
                return 0;
        }
    }
    return 1;
}
'''


_ADAPTERS = {
    "anf": _ADAPTER_ANF,
    "memory-ssa": _ADAPTER_MEMORY_SSA,
    "typed-tac": _ADAPTER_TYPED_TAC,
    "untyped-tac": _ADAPTER_UNTYPED_TAC,
    "gated": _ADAPTER_GATED,
    "sea-of-nodes": _ADAPTER_SEA,
    "cps": _ADAPTER_CPS,
    "supercombinators": _ADAPTER_SUPERCOMBINATORS,
}


__all__ = ["ALIASES", "NAME", "STAGE_ORDER", "emit"]
