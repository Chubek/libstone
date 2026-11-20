"""Generate a conservative, multidimensional C99 cost model.

The schema-v1 ArchDB has instruction encodings and derived architectural
effects, but it does not yet have a microarchitecture description.  This
stage consequently emits exact encoding sizes and instruction counts, keeps
the state-effect inventory, and represents unavailable quantities explicitly
as unknown.  It never turns missing latency, throughput, or register-pressure
data into a zero cost.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
import hashlib
import re

from ..diagnostics import ArchonError


NAME = "costmodel"
ALIASES = ("cost-model", "cost")
STAGE_ORDER = 300

_EFFECTS = (
    ("reads", "ARCHON_COST_STATE_REGISTER_READ", "ARCHON_COST_EFFECT_REGISTER_READ"),
    ("writes", "ARCHON_COST_STATE_REGISTER_WRITE", "ARCHON_COST_EFFECT_REGISTER_WRITE"),
    ("state_reads", "ARCHON_COST_STATE_SCALAR_READ", "ARCHON_COST_EFFECT_SCALAR_READ"),
    ("state_writes", "ARCHON_COST_STATE_SCALAR_WRITE", "ARCHON_COST_EFFECT_SCALAR_WRITE"),
    ("stack_reads", "ARCHON_COST_STATE_STACK_READ", "ARCHON_COST_EFFECT_STACK_READ"),
    ("stack_writes", "ARCHON_COST_STATE_STACK_WRITE", "ARCHON_COST_EFFECT_STACK_WRITE"),
    ("memory_reads", "ARCHON_COST_STATE_MEMORY_READ", "ARCHON_COST_EFFECT_MEMORY_READ"),
    ("memory_writes", "ARCHON_COST_STATE_MEMORY_WRITE", "ARCHON_COST_EFFECT_MEMORY_WRITE"),
)
_PROFILE_NAMES = {
    "size": "size",
    "speed": "speed",
    "balanced": "balanced",
    "oz": "Oz",
    "o3": "O3",
}


def _c_string(value: object) -> str:
    """Return an ASCII C string literal with unambiguous byte escapes."""

    result = ['"']
    for byte in str(value).encode("utf-8"):
        if byte == 0x22:
            result.append(r"\"")
        elif byte == 0x5C:
            result.append(r"\\")
        elif byte == 0x0A:
            result.append(r"\n")
        elif byte == 0x0D:
            result.append(r"\r")
        elif byte == 0x09:
            result.append(r"\t")
        # Escape '?' so C trigraph replacement cannot alter generated text.
        elif 0x20 <= byte < 0x7F and byte != 0x3F:
            result.append(chr(byte))
        else:
            result.append(f"\\{byte:03o}")
    result.append('"')
    return "".join(result)


def _filename_component(value: object) -> str:
    original = str(value)
    text = re.sub(r"[^A-Za-z0-9_]+", "_", original)
    text = re.sub(r"_+", "_", text).strip("_") or "architecture"
    if text[0].isdigit():
        text = "_" + text
    if text != original:
        text += "_" + hashlib.sha256(original.encode("utf-8")).hexdigest()[:12]
    return text


def _selected_databases(context: object) -> list[dict]:
    """Accept the canonical generator context and convenient direct forms."""

    if context is None:
        return []
    if isinstance(context, Mapping):
        if "architecture" in context and "instructions" in context:
            value: object = [context]
        else:
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
    elif isinstance(context, Sequence) and not isinstance(context, (str, bytes)):
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

    if isinstance(value, Mapping):
        value = value.values()
    try:
        entries = list(value)
    except TypeError as exc:
        raise ArchonError("cost-model generation requires selected ArchDB objects") from exc

    databases: list[dict] = []
    for index, entry in enumerate(entries):
        database: object
        if isinstance(entry, Mapping):
            database = entry
        elif (
            isinstance(entry, tuple)
            and len(entry) == 2
            and isinstance(entry[1], Mapping)
        ):
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
            raise ArchonError(
                f"cost-model selected architecture {index} is not an ArchDB object"
            )
        if not isinstance(database.get("architecture"), str):
            raise ArchonError(
                f"cost-model selected architecture {index} has no architecture name"
            )
        if not isinstance(database.get("instructions"), list):
            raise ArchonError(
                f"cost-model architecture {database['architecture']!r} has no instruction list"
            )
        databases.append(dict(database))

    return databases


def _default_profile(context: object) -> str:
    options = context.get("options", {}) if isinstance(context, Mapping) else {}
    if not isinstance(options, Mapping):
        return "size"
    raw = options.get("cost_profile", "size")
    if not isinstance(raw, str) or raw.lower() not in _PROFILE_NAMES:
        choices = ", ".join(("size", "speed", "balanced", "Oz", "O3"))
        raise ArchonError(
            f"unknown cost profile {raw!r}; expected one of: {choices}"
        )
    return _PROFILE_NAMES[raw.lower()]


def _effect_inventory(semantics: object) -> tuple[list[tuple[str, str]], list[str]]:
    """Return state-effect rows and flag constants."""

    if not isinstance(semantics, Mapping):
        return [("ARCHON_COST_STATE_UNKNOWN", "")], ["ARCHON_COST_EFFECT_UNKNOWN"]
    effects = semantics.get("effects")
    if not isinstance(effects, Mapping):
        return [("ARCHON_COST_STATE_UNKNOWN", "")], ["ARCHON_COST_EFFECT_UNKNOWN"]

    rows: list[tuple[str, str]] = []
    flags: list[str] = []
    for key, kind, flag in _EFFECTS:
        raw = effects.get(key, [])
        if not isinstance(raw, list) or any(not isinstance(name, str) for name in raw):
            return [("ARCHON_COST_STATE_UNKNOWN", "")], [
                "ARCHON_COST_EFFECT_UNKNOWN"
            ]
        names = sorted(set(raw))
        if names:
            flags.append(flag)
        rows.extend((kind, name) for name in names)
    return rows, flags


def _inventory(databases: list[dict]) -> list[dict]:
    architectures: list[dict] = []
    filenames: set[str] = set()
    instruction_index = 0
    effect_index = 0

    for architecture_index, database in enumerate(databases):
        architecture = database["architecture"]
        stem = f"Cost_{_filename_component(architecture)}"
        filename = f"{stem}.i"
        suffix = 2
        while filename.casefold() in filenames:
            filename = f"{stem}_{suffix}.i"
            suffix += 1
        filenames.add(filename.casefold())
        first_instruction = instruction_index
        first_arch_effect = effect_index
        instructions: list[dict] = []
        raw_instructions = database["instructions"]
        for raw in sorted(
            raw_instructions,
            key=lambda row: (
                str(row.get("name", "")) if isinstance(row, Mapping) else "",
                str(row.get("id", "")) if isinstance(row, Mapping) else "",
            ),
        ):
            if not isinstance(raw, Mapping):
                raise ArchonError(
                    f"cost-model architecture {architecture!r} has an invalid instruction"
                )
            name = raw.get("name")
            identifier = raw.get("id")
            width = raw.get("width")
            if (
                not isinstance(name, str)
                or not isinstance(identifier, str)
                or type(width) is not int
                or width <= 0
                or width % 8
            ):
                raise ArchonError(
                    f"cost-model architecture {architecture!r} has invalid instruction metadata"
                )

            effect_rows, effect_flags = _effect_inventory(raw.get("semantics"))
            first_effect = effect_index
            effects_with_indexes = []
            for kind, resource in effect_rows:
                effects_with_indexes.append(
                    {
                        "instruction_index": instruction_index,
                        "kind": kind,
                        "resource": resource,
                    }
                )
                effect_index += 1
            instructions.append(
                {
                    "architecture": architecture,
                    "architecture_index": architecture_index,
                    "index": instruction_index,
                    "id": identifier,
                    "name": name,
                    "code_bytes": width // 8,
                    # The state-effect set says which architectural memories
                    # may be touched.  It is not a cache/bus traffic model.
                    "memory_traffic": None,
                    "effect_flags": (
                        " | ".join(effect_flags)
                        if effect_flags
                        else "ARCHON_COST_EFFECT_NONE"
                    ),
                    "first_effect": first_effect,
                    "effects": effects_with_indexes,
                }
            )
            instruction_index += 1

        architectures.append(
            {
                "name": architecture,
                "filename": filename,
                "index": architecture_index,
                "first_instruction": first_instruction,
                "instruction_count": len(instructions),
                "first_effect": first_arch_effect,
                "effect_count": effect_index - first_arch_effect,
                "instructions": instructions,
            }
        )
    return architectures


def _known(value: int | None) -> tuple[int, int]:
    return (0, 0) if value is None else (1, value)


def _fragment(architecture: dict) -> str:
    lines = [
        "/* Generated by Archon. This file is a declarative cost-table include. */",
        "#if !defined(ARCHON_COST_ARCHITECTURE_BEGIN) \\",
        " || !defined(ARCHON_COST_INSTRUCTION) \\",
        " || !defined(ARCHON_COST_STATE_EFFECT) \\",
        " || !defined(ARCHON_COST_ARCHITECTURE_END)",
        '#error "define the complete Archon cost-table macro schema before inclusion"',
        "#endif",
        "ARCHON_COST_ARCHITECTURE_BEGIN("
        f"{_c_string(architecture['name'])}, "
        f"{architecture['first_instruction']}u, {architecture['instruction_count']}u, "
        f"{architecture['first_effect']}u, {architecture['effect_count']}u)",
    ]
    for instruction in architecture["instructions"]:
        memory_known, memory_value = _known(instruction["memory_traffic"])
        lines.extend(
            [
                "ARCHON_COST_INSTRUCTION(",
                f"    {instruction['architecture_index']}u, {instruction['index']}u,",
                f"    {_c_string(instruction['architecture'])}, "
                f"{_c_string(instruction['id'])}, {_c_string(instruction['name'])},",
                f"    1u, UINT64_C({instruction['code_bytes']}), "
                "1u, UINT64_C(1),",
                "    0u, UINT64_C(0), 0u, UINT64_C(0),",
                "    0u, UINT64_C(0), "
                f"{memory_known}u, UINT64_C({memory_value}),",
                f"    {instruction['effect_flags']}, "
                f"{instruction['first_effect']}u, {len(instruction['effects'])}u)",
            ]
        )
        for effect in instruction["effects"]:
            lines.append(
                "ARCHON_COST_STATE_EFFECT("
                f"{effect['instruction_index']}u, {effect['kind']}, "
                f"{_c_string(effect['resource'])})"
            )
    lines.append("ARCHON_COST_ARCHITECTURE_END()")
    return "\n".join(lines) + "\n"


def emit(context: object) -> dict[str, str]:
    """Emit ``CostModel.c``, ``CostModel.h``, and one table per ArchDB."""

    architectures = _inventory(_selected_databases(context))
    filenames = [architecture["filename"] for architecture in architectures]
    instruction_count = sum(row["instruction_count"] for row in architectures)
    effect_count = sum(row["effect_count"] for row in architectures)
    includes = "\n".join(f'#include "{filename}"' for filename in filenames)
    replacements = {
        "@ARCHITECTURE_COUNT@": str(len(architectures)),
        "@INSTRUCTION_COUNT@": str(instruction_count),
        "@EFFECT_COUNT@": str(effect_count),
        "@COST_TABLE_INCLUDES@": includes,
        "@DEFAULT_PROFILE@": _c_string(_default_profile(context)),
    }

    source = _SOURCE
    for token, value in replacements.items():
        source = source.replace(token, value)
    if re.search(r"@[A-Z_]+@", source):
        raise AssertionError("unexpanded cost-model template token")

    outputs = {"CostModel.c": source, "CostModel.h": _HEADER}
    for architecture in architectures:
        outputs[architecture["filename"]] = _fragment(architecture)
    return dict(sorted(outputs.items()))


_HEADER = r"""/* Generated by Archon: conservative multidimensional cost model. */
#ifndef ARCHON_GENERATED_COST_MODEL_H
#define ARCHON_GENERATED_COST_MODEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ArchonCostStatus {
    ARCHON_COST_OK = 0,
    ARCHON_COST_INVALID_ARGUMENT,
    ARCHON_COST_UNAVAILABLE,
    ARCHON_COST_OVERFLOW,
    ARCHON_COST_NOT_FOUND
} ArchonCostStatus;

typedef enum ArchonCostDimension {
    ARCHON_COST_CODE_BYTES = 0,
    ARCHON_COST_INSTRUCTION_COUNT,
    ARCHON_COST_LATENCY,
    ARCHON_COST_THROUGHPUT,
    ARCHON_COST_REGISTER_PRESSURE,
    ARCHON_COST_MEMORY_TRAFFIC,
    ARCHON_COST_DIMENSION_COUNT
} ArchonCostDimension;

#define ARCHON_COST_DIM_CODE_BYTES ARCHON_COST_CODE_BYTES
#define ARCHON_COST_DIM_INSTRUCTION_COUNT ARCHON_COST_INSTRUCTION_COUNT
#define ARCHON_COST_DIM_LATENCY ARCHON_COST_LATENCY
#define ARCHON_COST_DIM_THROUGHPUT ARCHON_COST_THROUGHPUT
#define ARCHON_COST_DIM_REGISTER_PRESSURE ARCHON_COST_REGISTER_PRESSURE
#define ARCHON_COST_DIM_MEMORY_TRAFFIC ARCHON_COST_MEMORY_TRAFFIC
#define ARCHON_COST_DIMENSION_MASK(DIMENSION) \
    (UINT32_C(1) << (unsigned)(DIMENSION))
#define ARCHON_COST_ALL_DIMENSIONS \
    ((UINT32_C(1) << (unsigned)ARCHON_COST_DIMENSION_COUNT) - UINT32_C(1))

typedef struct ArchonCostValue {
    uint64_t value;
    unsigned known;
} ArchonCostValue;

#define ARCHON_COST_KNOWN(VALUE) \
    ((ArchonCostValue){(uint64_t)(VALUE), 1u})
#define ARCHON_COST_UNKNOWN \
    ((ArchonCostValue){UINT64_C(0), 0u})

typedef struct ArchonCost {
    ArchonCostValue code_bytes;
    ArchonCostValue instruction_count;
    ArchonCostValue latency;
    ArchonCostValue throughput;
    ArchonCostValue register_pressure;
    ArchonCostValue memory_traffic;
} ArchonCost;

typedef enum ArchonCostRelation {
    ARCHON_COST_BETTER = -1,
    ARCHON_COST_EQUAL = 0,
    ARCHON_COST_WORSE = 1,
    ARCHON_COST_INCOMPARABLE = 2,
    ARCHON_COST_RELATION_UNKNOWN = 3
} ArchonCostRelation;

typedef enum ArchonCostObjectiveKind {
    ARCHON_COST_OBJECTIVE_WEIGHTED = 1,
    ARCHON_COST_OBJECTIVE_LEXICOGRAPHIC = 2
} ArchonCostObjectiveKind;

/*
 * Weighted objectives are normalized explicitly:
 *
 *   score = sum(weight[d] * cost[d] / normalizer[d]) / sum(weight[d])
 *
 * Zero-weight dimensions are ignored.  Every active dimension must be known,
 * every active weight must be finite and nonnegative, and every active
 * normalizer must be finite and greater than zero.  Lexicographic objectives
 * use order[0..order_count), with lower values preferred.
 */
typedef struct ArchonCostProfile {
    const char *name;
    ArchonCostObjectiveKind objective;
    double weight[ARCHON_COST_DIMENSION_COUNT];
    double normalizer[ARCHON_COST_DIMENSION_COUNT];
    ArchonCostDimension order[ARCHON_COST_DIMENSION_COUNT];
    size_t order_count;
} ArchonCostProfile;

typedef enum ArchonCostStateEffectKind {
    ARCHON_COST_STATE_REGISTER_READ = 1,
    ARCHON_COST_STATE_REGISTER_WRITE,
    ARCHON_COST_STATE_SCALAR_READ,
    ARCHON_COST_STATE_SCALAR_WRITE,
    ARCHON_COST_STATE_STACK_READ,
    ARCHON_COST_STATE_STACK_WRITE,
    ARCHON_COST_STATE_MEMORY_READ,
    ARCHON_COST_STATE_MEMORY_WRITE,
    ARCHON_COST_STATE_UNKNOWN
} ArchonCostStateEffectKind;

typedef enum ArchonCostEffectFlags {
    ARCHON_COST_EFFECT_NONE = 0,
    ARCHON_COST_EFFECT_REGISTER_READ = 1u << 0,
    ARCHON_COST_EFFECT_REGISTER_WRITE = 1u << 1,
    ARCHON_COST_EFFECT_SCALAR_READ = 1u << 2,
    ARCHON_COST_EFFECT_SCALAR_WRITE = 1u << 3,
    ARCHON_COST_EFFECT_STACK_READ = 1u << 4,
    ARCHON_COST_EFFECT_STACK_WRITE = 1u << 5,
    ARCHON_COST_EFFECT_MEMORY_READ = 1u << 6,
    ARCHON_COST_EFFECT_MEMORY_WRITE = 1u << 7,
    ARCHON_COST_EFFECT_UNKNOWN = 1u << 8
} ArchonCostEffectFlags;

typedef struct ArchonCostArchitecture {
    const char *name;
    size_t first_instruction;
    size_t instruction_count;
    size_t first_effect;
    size_t effect_count;
} ArchonCostArchitecture;

typedef struct ArchonInstructionCost {
    size_t architecture_index;
    size_t index;
    const char *architecture;
    const char *id;
    const char *name;
    ArchonCost cost;
    uint32_t effect_flags;
    size_t first_effect;
    size_t effect_count;
} ArchonInstructionCost;

typedef struct ArchonCostStateEffect {
    size_t instruction_index;
    ArchonCostStateEffectKind kind;
    const char *resource;
} ArchonCostStateEffect;

ArchonCostValue archon_cost_value_known(uint64_t value);
ArchonCostValue archon_cost_value_unknown(void);
int archon_cost_value_is_known(ArchonCostValue value);
ArchonCost archon_cost_zero(void);
ArchonCost archon_cost_all_unknown(void);
const ArchonCostValue *archon_cost_value(
    const ArchonCost *cost, ArchonCostDimension dimension);
ArchonCostValue *archon_cost_value_mutable(
    ArchonCost *cost, ArchonCostDimension dimension);
const char *archon_cost_dimension_name(ArchonCostDimension dimension);
const char *archon_cost_status_name(ArchonCostStatus status);
const char *archon_cost_state_effect_name(ArchonCostStateEffectKind kind);

/*
 * Sequence aggregation is deterministic integer arithmetic.  Code bytes,
 * instruction count, latency, reciprocal throughput, and memory traffic add;
 * register pressure takes the maximum.  Unknown operands propagate.  An
 * overflowing sum becomes unknown and the function reports OVERFLOW.
 */
ArchonCostStatus archon_cost_sequence_append(
    ArchonCost *aggregate, const ArchonCost *instruction);
ArchonCostStatus archon_cost_sequence(
    const ArchonCost *costs, size_t count, ArchonCost *aggregate);
ArchonCostStatus archon_cost_aggregate_sequence(
    const ArchonCost *costs, size_t count, ArchonCost *aggregate);

ArchonCostStatus archon_cost_pareto_mask(
    const ArchonCost *left, const ArchonCost *right, uint32_t dimensions,
    ArchonCostRelation *relation);
ArchonCostRelation archon_cost_pareto_compare(
    const ArchonCost *left, const ArchonCost *right);

ArchonCostStatus archon_cost_profile_init_weighted(
    ArchonCostProfile *profile, const char *name,
    const double weights[ARCHON_COST_DIMENSION_COUNT],
    const double normalizers[ARCHON_COST_DIMENSION_COUNT]);
ArchonCostStatus archon_cost_profile_init_lexicographic(
    ArchonCostProfile *profile, const char *name,
    const ArchonCostDimension *order, size_t order_count);
ArchonCostStatus archon_cost_normalized_weighted_score(
    const ArchonCost *cost, const ArchonCostProfile *profile, double *score);
ArchonCostStatus archon_cost_score(
    const ArchonCost *cost, const ArchonCostProfile *profile, double *score);
ArchonCostStatus archon_cost_compare(
    const ArchonCost *left, const ArchonCost *right,
    const ArchonCostProfile *profile, ArchonCostRelation *relation);
ArchonCostStatus archon_cost_select_best(
    const ArchonCost *costs, size_t count, const ArchonCostProfile *profile,
    size_t *selected);

size_t archon_cost_profile_count(void);
const ArchonCostProfile *archon_cost_profile(size_t index);
const ArchonCostProfile *archon_cost_profile_by_name(const char *name);
const ArchonCostProfile *archon_cost_select_profile(const char *name);
const ArchonCostProfile *archon_cost_default_profile(void);

size_t archon_cost_architecture_count(void);
const ArchonCostArchitecture *archon_cost_architecture(size_t index);
size_t archon_cost_instruction_count(void);
const ArchonInstructionCost *archon_cost_instruction(size_t index);
const ArchonInstructionCost *archon_cost_find_instruction(
    const char *architecture, const char *name);
const ArchonInstructionCost *archon_cost_instruction_by_name(
    const char *architecture, const char *name);
size_t archon_cost_state_effect_count(void);
const ArchonCostStateEffect *archon_cost_state_effect(size_t index);
const ArchonCostStateEffect *archon_cost_instruction_effect(
    const ArchonInstructionCost *instruction, size_t index);

/*
 * Cost_<architecture>.i files intentionally have no include guard.  Consumers
 * define these four macros before inclusion:
 *
 * ARCHON_COST_ARCHITECTURE_BEGIN(
 *     name, first_instruction, instruction_count, first_effect, effect_count)
 * ARCHON_COST_INSTRUCTION(
 *     architecture_index, instruction_index, architecture, id, name,
 *     code_known, code_value, count_known, count_value,
 *     latency_known, latency_value, throughput_known, throughput_value,
 *     pressure_known, pressure_value, memory_known, memory_value,
 *     effect_flags, first_effect, effect_count)
 * ARCHON_COST_STATE_EFFECT(instruction_index, kind, resource)
 * ARCHON_COST_ARCHITECTURE_END()
 */

#ifdef __cplusplus
}
#endif

#endif
"""


_SOURCE = r"""/* Generated by Archon: conservative multidimensional cost model. */
#include "CostModel.h"

#include <float.h>
#include <limits.h>
#include <string.h>

static const ArchonCostArchitecture archon_cost_architectures[] = {
#if @ARCHITECTURE_COUNT@ > 0
#define ARCHON_COST_ARCHITECTURE_BEGIN(NAME, FIRSTI, ICOUNT, FIRSTE, ECOUNT) \
    {(NAME), (FIRSTI), (ICOUNT), (FIRSTE), (ECOUNT)},
#define ARCHON_COST_INSTRUCTION(AI, II, ARCH, ID, NAME, CK, CV, IK, IV, \
    LK, LV, TK, TV, PK, PV, MK, MV, FLAGS, FIRST, COUNT)
#define ARCHON_COST_STATE_EFFECT(INSTRUCTION, KIND, RESOURCE)
#define ARCHON_COST_ARCHITECTURE_END()
@COST_TABLE_INCLUDES@
#undef ARCHON_COST_ARCHITECTURE_END
#undef ARCHON_COST_STATE_EFFECT
#undef ARCHON_COST_INSTRUCTION
#undef ARCHON_COST_ARCHITECTURE_BEGIN
#else
    {NULL, 0u, 0u, 0u, 0u},
#endif
};

static const ArchonInstructionCost archon_cost_instructions[] = {
#if @INSTRUCTION_COUNT@ > 0
#define ARCHON_COST_ARCHITECTURE_BEGIN(NAME, FIRSTI, ICOUNT, FIRSTE, ECOUNT)
#define ARCHON_COST_INSTRUCTION(AI, II, ARCH, ID, NAME, CK, CV, IK, IV, \
        LK, LV, TK, TV, PK, PV, MK, MV, FLAGS, FIRST, COUNT) \
    {(AI), (II), (ARCH), (ID), (NAME), \
     {{(CV), (CK)}, {(IV), (IK)}, {(LV), (LK)}, {(TV), (TK)}, \
      {(PV), (PK)}, {(MV), (MK)}}, \
     (uint32_t)(FLAGS), (FIRST), (COUNT)},
#define ARCHON_COST_STATE_EFFECT(INSTRUCTION, KIND, RESOURCE)
#define ARCHON_COST_ARCHITECTURE_END()
@COST_TABLE_INCLUDES@
#undef ARCHON_COST_ARCHITECTURE_END
#undef ARCHON_COST_STATE_EFFECT
#undef ARCHON_COST_INSTRUCTION
#undef ARCHON_COST_ARCHITECTURE_BEGIN
#else
    {0u, 0u, NULL, NULL, NULL,
     {{0u, 0u}, {0u, 0u}, {0u, 0u}, {0u, 0u}, {0u, 0u}, {0u, 0u}},
     0u, 0u, 0u},
#endif
};

static const ArchonCostStateEffect archon_cost_effects[] = {
#if @EFFECT_COUNT@ > 0
#define ARCHON_COST_ARCHITECTURE_BEGIN(NAME, FIRSTI, ICOUNT, FIRSTE, ECOUNT)
#define ARCHON_COST_INSTRUCTION(AI, II, ARCH, ID, NAME, CK, CV, IK, IV, \
    LK, LV, TK, TV, PK, PV, MK, MV, FLAGS, FIRST, COUNT)
#define ARCHON_COST_STATE_EFFECT(INSTRUCTION, KIND, RESOURCE) \
    {(INSTRUCTION), (KIND), (RESOURCE)},
#define ARCHON_COST_ARCHITECTURE_END()
@COST_TABLE_INCLUDES@
#undef ARCHON_COST_ARCHITECTURE_END
#undef ARCHON_COST_STATE_EFFECT
#undef ARCHON_COST_INSTRUCTION
#undef ARCHON_COST_ARCHITECTURE_BEGIN
#else
    {0u, ARCHON_COST_STATE_UNKNOWN, NULL},
#endif
};

static const size_t archon_cost_architecture_table_count = @ARCHITECTURE_COUNT@u;
static const size_t archon_cost_instruction_table_count = @INSTRUCTION_COUNT@u;
static const size_t archon_cost_effect_table_count = @EFFECT_COUNT@u;
static const char archon_cost_default_profile_name[] = @DEFAULT_PROFILE@;

static const ArchonCostProfile archon_cost_profiles[] = {
    {
        "size", ARCHON_COST_OBJECTIVE_LEXICOGRAPHIC,
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {1.0, 1.0, 1.0, 1.0, 1.0, 1.0},
        {ARCHON_COST_CODE_BYTES, ARCHON_COST_INSTRUCTION_COUNT,
         ARCHON_COST_MEMORY_TRAFFIC, ARCHON_COST_REGISTER_PRESSURE,
         ARCHON_COST_LATENCY, ARCHON_COST_THROUGHPUT},
        2u
    },
    {
        "speed", ARCHON_COST_OBJECTIVE_LEXICOGRAPHIC,
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
        {1.0, 1.0, 1.0, 1.0, 1.0, 1.0},
        {ARCHON_COST_LATENCY, ARCHON_COST_THROUGHPUT,
         ARCHON_COST_REGISTER_PRESSURE, ARCHON_COST_MEMORY_TRAFFIC,
         ARCHON_COST_CODE_BYTES, ARCHON_COST_INSTRUCTION_COUNT},
        ARCHON_COST_DIMENSION_COUNT
    },
    {
        "balanced", ARCHON_COST_OBJECTIVE_WEIGHTED,
        {0.20, 0.10, 0.25, 0.15, 0.15, 0.15},
        {16.0, 4.0, 10.0, 10.0, 8.0, 64.0},
        {ARCHON_COST_CODE_BYTES, ARCHON_COST_INSTRUCTION_COUNT,
         ARCHON_COST_LATENCY, ARCHON_COST_THROUGHPUT,
         ARCHON_COST_REGISTER_PRESSURE, ARCHON_COST_MEMORY_TRAFFIC},
        ARCHON_COST_DIMENSION_COUNT
    },
    {
        "Oz", ARCHON_COST_OBJECTIVE_WEIGHTED,
        {0.80, 0.20, 0.0, 0.0, 0.0, 0.0},
        {16.0, 4.0, 1.0, 1.0, 1.0, 1.0},
        {ARCHON_COST_CODE_BYTES, ARCHON_COST_INSTRUCTION_COUNT,
         ARCHON_COST_LATENCY, ARCHON_COST_THROUGHPUT,
         ARCHON_COST_REGISTER_PRESSURE, ARCHON_COST_MEMORY_TRAFFIC},
        ARCHON_COST_DIMENSION_COUNT
    },
    {
        "O3", ARCHON_COST_OBJECTIVE_WEIGHTED,
        {0.05, 0.0, 0.50, 0.30, 0.15, 0.0},
        {16.0, 1.0, 10.0, 10.0, 8.0, 1.0},
        {ARCHON_COST_CODE_BYTES, ARCHON_COST_INSTRUCTION_COUNT,
         ARCHON_COST_LATENCY, ARCHON_COST_THROUGHPUT,
         ARCHON_COST_REGISTER_PRESSURE, ARCHON_COST_MEMORY_TRAFFIC},
        ARCHON_COST_DIMENSION_COUNT
    }
};

static const size_t archon_cost_profile_table_count =
    sizeof archon_cost_profiles / sizeof archon_cost_profiles[0];

ArchonCostValue archon_cost_value_known(uint64_t value) {
    ArchonCostValue result;
    result.value = value;
    result.known = 1u;
    return result;
}

ArchonCostValue archon_cost_value_unknown(void) {
    ArchonCostValue result;
    result.value = UINT64_C(0);
    result.known = 0u;
    return result;
}

int archon_cost_value_is_known(ArchonCostValue value) {
    return value.known != 0u;
}

ArchonCost archon_cost_zero(void) {
    ArchonCost result;
    result.code_bytes = archon_cost_value_known(UINT64_C(0));
    result.instruction_count = archon_cost_value_known(UINT64_C(0));
    result.latency = archon_cost_value_known(UINT64_C(0));
    result.throughput = archon_cost_value_known(UINT64_C(0));
    result.register_pressure = archon_cost_value_known(UINT64_C(0));
    result.memory_traffic = archon_cost_value_known(UINT64_C(0));
    return result;
}

ArchonCost archon_cost_all_unknown(void) {
    ArchonCost result;
    result.code_bytes = archon_cost_value_unknown();
    result.instruction_count = archon_cost_value_unknown();
    result.latency = archon_cost_value_unknown();
    result.throughput = archon_cost_value_unknown();
    result.register_pressure = archon_cost_value_unknown();
    result.memory_traffic = archon_cost_value_unknown();
    return result;
}

const ArchonCostValue *archon_cost_value(
        const ArchonCost *cost, ArchonCostDimension dimension) {
    if (cost == NULL)
        return NULL;
    switch (dimension) {
    case ARCHON_COST_CODE_BYTES: return &cost->code_bytes;
    case ARCHON_COST_INSTRUCTION_COUNT: return &cost->instruction_count;
    case ARCHON_COST_LATENCY: return &cost->latency;
    case ARCHON_COST_THROUGHPUT: return &cost->throughput;
    case ARCHON_COST_REGISTER_PRESSURE: return &cost->register_pressure;
    case ARCHON_COST_MEMORY_TRAFFIC: return &cost->memory_traffic;
    default: return NULL;
    }
}

ArchonCostValue *archon_cost_value_mutable(
        ArchonCost *cost, ArchonCostDimension dimension) {
    if (cost == NULL)
        return NULL;
    switch (dimension) {
    case ARCHON_COST_CODE_BYTES: return &cost->code_bytes;
    case ARCHON_COST_INSTRUCTION_COUNT: return &cost->instruction_count;
    case ARCHON_COST_LATENCY: return &cost->latency;
    case ARCHON_COST_THROUGHPUT: return &cost->throughput;
    case ARCHON_COST_REGISTER_PRESSURE: return &cost->register_pressure;
    case ARCHON_COST_MEMORY_TRAFFIC: return &cost->memory_traffic;
    default: return NULL;
    }
}

const char *archon_cost_dimension_name(ArchonCostDimension dimension) {
    switch (dimension) {
    case ARCHON_COST_CODE_BYTES: return "code-bytes";
    case ARCHON_COST_INSTRUCTION_COUNT: return "instruction-count";
    case ARCHON_COST_LATENCY: return "latency";
    case ARCHON_COST_THROUGHPUT: return "throughput";
    case ARCHON_COST_REGISTER_PRESSURE: return "register-pressure";
    case ARCHON_COST_MEMORY_TRAFFIC: return "memory-traffic";
    default: return "invalid";
    }
}

const char *archon_cost_status_name(ArchonCostStatus status) {
    switch (status) {
    case ARCHON_COST_OK: return "ok";
    case ARCHON_COST_INVALID_ARGUMENT: return "invalid-argument";
    case ARCHON_COST_UNAVAILABLE: return "unavailable";
    case ARCHON_COST_OVERFLOW: return "overflow";
    case ARCHON_COST_NOT_FOUND: return "not-found";
    default: return "invalid";
    }
}

const char *archon_cost_state_effect_name(ArchonCostStateEffectKind kind) {
    switch (kind) {
    case ARCHON_COST_STATE_REGISTER_READ: return "register-read";
    case ARCHON_COST_STATE_REGISTER_WRITE: return "register-write";
    case ARCHON_COST_STATE_SCALAR_READ: return "scalar-read";
    case ARCHON_COST_STATE_SCALAR_WRITE: return "scalar-write";
    case ARCHON_COST_STATE_STACK_READ: return "stack-read";
    case ARCHON_COST_STATE_STACK_WRITE: return "stack-write";
    case ARCHON_COST_STATE_MEMORY_READ: return "memory-read";
    case ARCHON_COST_STATE_MEMORY_WRITE: return "memory-write";
    case ARCHON_COST_STATE_UNKNOWN: return "unknown";
    default: return "invalid";
    }
}

static ArchonCostStatus archon_cost_add_value(
        ArchonCostValue left, ArchonCostValue right, ArchonCostValue *result) {
    if (left.known == 0u || right.known == 0u) {
        *result = archon_cost_value_unknown();
        return ARCHON_COST_OK;
    }
    if (right.value > UINT64_MAX - left.value) {
        *result = archon_cost_value_unknown();
        return ARCHON_COST_OVERFLOW;
    }
    *result = archon_cost_value_known(left.value + right.value);
    return ARCHON_COST_OK;
}

static ArchonCostValue archon_cost_max_value(
        ArchonCostValue left, ArchonCostValue right) {
    if (left.known == 0u || right.known == 0u)
        return archon_cost_value_unknown();
    return archon_cost_value_known(left.value > right.value ? left.value : right.value);
}

ArchonCostStatus archon_cost_sequence_append(
        ArchonCost *aggregate, const ArchonCost *instruction) {
    ArchonCost left;
    ArchonCost right;
    ArchonCost result;
    ArchonCostStatus status = ARCHON_COST_OK;
    ArchonCostStatus current;
    if (aggregate == NULL || instruction == NULL)
        return ARCHON_COST_INVALID_ARGUMENT;
    left = *aggregate;
    right = *instruction;
    current = archon_cost_add_value(
        left.code_bytes, right.code_bytes, &result.code_bytes);
    if (current != ARCHON_COST_OK)
        status = current;
    current = archon_cost_add_value(
        left.instruction_count, right.instruction_count,
        &result.instruction_count);
    if (current != ARCHON_COST_OK)
        status = current;
    current = archon_cost_add_value(left.latency, right.latency, &result.latency);
    if (current != ARCHON_COST_OK)
        status = current;
    current = archon_cost_add_value(
        left.throughput, right.throughput, &result.throughput);
    if (current != ARCHON_COST_OK)
        status = current;
    result.register_pressure = archon_cost_max_value(
        left.register_pressure, right.register_pressure);
    current = archon_cost_add_value(
        left.memory_traffic, right.memory_traffic, &result.memory_traffic);
    if (current != ARCHON_COST_OK)
        status = current;
    *aggregate = result;
    return status;
}

ArchonCostStatus archon_cost_sequence(
        const ArchonCost *costs, size_t count, ArchonCost *aggregate) {
    ArchonCost result;
    ArchonCostStatus status = ARCHON_COST_OK;
    size_t index;
    if (aggregate == NULL || (count != 0u && costs == NULL))
        return ARCHON_COST_INVALID_ARGUMENT;
    result = archon_cost_zero();
    for (index = 0u; index < count; ++index) {
        ArchonCostStatus current = archon_cost_sequence_append(&result, &costs[index]);
        if (current == ARCHON_COST_INVALID_ARGUMENT)
            return current;
        if (current == ARCHON_COST_OVERFLOW)
            status = current;
    }
    *aggregate = result;
    return status;
}

ArchonCostStatus archon_cost_aggregate_sequence(
        const ArchonCost *costs, size_t count, ArchonCost *aggregate) {
    return archon_cost_sequence(costs, count, aggregate);
}

ArchonCostStatus archon_cost_pareto_mask(
        const ArchonCost *left, const ArchonCost *right, uint32_t dimensions,
        ArchonCostRelation *relation) {
    int left_better = 0;
    int right_better = 0;
    int unknown = 0;
    unsigned dimension;
    if (left == NULL || right == NULL || relation == NULL ||
            dimensions == 0u ||
            (dimensions & ~ARCHON_COST_ALL_DIMENSIONS) != 0u)
        return ARCHON_COST_INVALID_ARGUMENT;
    *relation = ARCHON_COST_RELATION_UNKNOWN;
    for (dimension = 0u;
            dimension < (unsigned)ARCHON_COST_DIMENSION_COUNT; ++dimension) {
        const ArchonCostValue *a;
        const ArchonCostValue *b;
        if ((dimensions & (UINT32_C(1) << dimension)) == 0u)
            continue;
        a = archon_cost_value(left, (ArchonCostDimension)dimension);
        b = archon_cost_value(right, (ArchonCostDimension)dimension);
        if (a->known == 0u || b->known == 0u) {
            unknown = 1;
        } else if (a->value < b->value) {
            left_better = 1;
        } else if (a->value > b->value) {
            right_better = 1;
        }
    }
    if (left_better && right_better) {
        *relation = ARCHON_COST_INCOMPARABLE;
        return ARCHON_COST_OK;
    }
    if (unknown)
        return ARCHON_COST_UNAVAILABLE;
    if (left_better)
        *relation = ARCHON_COST_BETTER;
    else if (right_better)
        *relation = ARCHON_COST_WORSE;
    else
        *relation = ARCHON_COST_EQUAL;
    return ARCHON_COST_OK;
}

ArchonCostRelation archon_cost_pareto_compare(
        const ArchonCost *left, const ArchonCost *right) {
    ArchonCostRelation relation = ARCHON_COST_RELATION_UNKNOWN;
    (void)archon_cost_pareto_mask(
        left, right, ARCHON_COST_ALL_DIMENSIONS, &relation);
    return relation;
}

static int archon_cost_finite_nonnegative(double value) {
    return value >= 0.0 && value <= DBL_MAX;
}

static int archon_cost_valid_dimension(ArchonCostDimension dimension) {
    return (unsigned)dimension < (unsigned)ARCHON_COST_DIMENSION_COUNT;
}

static ArchonCostStatus archon_cost_validate_profile(
        const ArchonCostProfile *profile) {
    size_t index;
    if (profile == NULL || profile->name == NULL)
        return ARCHON_COST_INVALID_ARGUMENT;
    if (profile->objective == ARCHON_COST_OBJECTIVE_WEIGHTED) {
        int active = 0;
        for (index = 0u; index < ARCHON_COST_DIMENSION_COUNT; ++index) {
            double weight = profile->weight[index];
            if (!archon_cost_finite_nonnegative(weight))
                return ARCHON_COST_INVALID_ARGUMENT;
            if (weight > 0.0) {
                double normalizer = profile->normalizer[index];
                if (!(normalizer > 0.0 && normalizer <= DBL_MAX))
                    return ARCHON_COST_INVALID_ARGUMENT;
                active = 1;
            }
        }
        return active ? ARCHON_COST_OK : ARCHON_COST_INVALID_ARGUMENT;
    }
    if (profile->objective == ARCHON_COST_OBJECTIVE_LEXICOGRAPHIC) {
        unsigned seen = 0u;
        if (profile->order_count == 0u ||
                profile->order_count > ARCHON_COST_DIMENSION_COUNT)
            return ARCHON_COST_INVALID_ARGUMENT;
        for (index = 0u; index < profile->order_count; ++index) {
            unsigned bit;
            if (!archon_cost_valid_dimension(profile->order[index]))
                return ARCHON_COST_INVALID_ARGUMENT;
            bit = 1u << (unsigned)profile->order[index];
            if ((seen & bit) != 0u)
                return ARCHON_COST_INVALID_ARGUMENT;
            seen |= bit;
        }
        return ARCHON_COST_OK;
    }
    return ARCHON_COST_INVALID_ARGUMENT;
}

ArchonCostStatus archon_cost_profile_init_weighted(
        ArchonCostProfile *profile, const char *name,
        const double weights[ARCHON_COST_DIMENSION_COUNT],
        const double normalizers[ARCHON_COST_DIMENSION_COUNT]) {
    ArchonCostProfile result;
    size_t index;
    if (profile == NULL || name == NULL || weights == NULL || normalizers == NULL)
        return ARCHON_COST_INVALID_ARGUMENT;
    memset(&result, 0, sizeof result);
    result.name = name;
    result.objective = ARCHON_COST_OBJECTIVE_WEIGHTED;
    result.order_count = ARCHON_COST_DIMENSION_COUNT;
    for (index = 0u; index < ARCHON_COST_DIMENSION_COUNT; ++index) {
        result.weight[index] = weights[index];
        result.normalizer[index] = normalizers[index];
        result.order[index] = (ArchonCostDimension)index;
    }
    if (archon_cost_validate_profile(&result) != ARCHON_COST_OK)
        return ARCHON_COST_INVALID_ARGUMENT;
    *profile = result;
    return ARCHON_COST_OK;
}

ArchonCostStatus archon_cost_profile_init_lexicographic(
        ArchonCostProfile *profile, const char *name,
        const ArchonCostDimension *order, size_t order_count) {
    ArchonCostProfile result;
    size_t index;
    if (profile == NULL || name == NULL || order == NULL)
        return ARCHON_COST_INVALID_ARGUMENT;
    memset(&result, 0, sizeof result);
    result.name = name;
    result.objective = ARCHON_COST_OBJECTIVE_LEXICOGRAPHIC;
    result.order_count = order_count;
    for (index = 0u; index < ARCHON_COST_DIMENSION_COUNT; ++index)
        result.normalizer[index] = 1.0;
    if (order_count <= ARCHON_COST_DIMENSION_COUNT)
        for (index = 0u; index < order_count; ++index)
            result.order[index] = order[index];
    if (archon_cost_validate_profile(&result) != ARCHON_COST_OK)
        return ARCHON_COST_INVALID_ARGUMENT;
    *profile = result;
    return ARCHON_COST_OK;
}

static ArchonCostStatus archon_cost_weighted_score_long(
        const ArchonCost *cost, const ArchonCostProfile *profile,
        long double *score) {
    long double numerator = 0.0L;
    long double denominator = 0.0L;
    size_t index;
    ArchonCostStatus valid;
    if (cost == NULL || score == NULL)
        return ARCHON_COST_INVALID_ARGUMENT;
    valid = archon_cost_validate_profile(profile);
    if (valid != ARCHON_COST_OK ||
            profile->objective != ARCHON_COST_OBJECTIVE_WEIGHTED)
        return ARCHON_COST_INVALID_ARGUMENT;
    for (index = 0u; index < ARCHON_COST_DIMENSION_COUNT; ++index) {
        long double weight;
        long double normalized;
        long double term;
        const ArchonCostValue *value;
        if (profile->weight[index] == 0.0)
            continue;
        value = archon_cost_value(cost, (ArchonCostDimension)index);
        if (value->known == 0u)
            return ARCHON_COST_UNAVAILABLE;
        weight = (long double)profile->weight[index];
        normalized = (long double)value->value /
            (long double)profile->normalizer[index];
        if (weight != 0.0L && normalized > LDBL_MAX / weight)
            return ARCHON_COST_OVERFLOW;
        term = weight * normalized;
        if (term > LDBL_MAX - numerator || weight > LDBL_MAX - denominator)
            return ARCHON_COST_OVERFLOW;
        numerator += term;
        denominator += weight;
    }
    if (denominator == 0.0L)
        return ARCHON_COST_INVALID_ARGUMENT;
    *score = numerator / denominator;
    return ARCHON_COST_OK;
}

ArchonCostStatus archon_cost_normalized_weighted_score(
        const ArchonCost *cost, const ArchonCostProfile *profile, double *score) {
    long double wide_score = 0.0L;
    ArchonCostStatus status;
    if (score == NULL)
        return ARCHON_COST_INVALID_ARGUMENT;
    *score = 0.0;
    status = archon_cost_weighted_score_long(cost, profile, &wide_score);
    if (status != ARCHON_COST_OK)
        return status;
    if (wide_score > (long double)DBL_MAX)
        return ARCHON_COST_OVERFLOW;
    *score = (double)wide_score;
    return ARCHON_COST_OK;
}

ArchonCostStatus archon_cost_score(
        const ArchonCost *cost, const ArchonCostProfile *profile, double *score) {
    return archon_cost_normalized_weighted_score(cost, profile, score);
}

ArchonCostStatus archon_cost_compare(
        const ArchonCost *left, const ArchonCost *right,
        const ArchonCostProfile *profile, ArchonCostRelation *relation) {
    ArchonCostStatus status;
    if (left == NULL || right == NULL || relation == NULL)
        return ARCHON_COST_INVALID_ARGUMENT;
    *relation = ARCHON_COST_RELATION_UNKNOWN;
    status = archon_cost_validate_profile(profile);
    if (status != ARCHON_COST_OK)
        return status;
    if (profile->objective == ARCHON_COST_OBJECTIVE_WEIGHTED) {
        long double left_score;
        long double right_score;
        status = archon_cost_weighted_score_long(left, profile, &left_score);
        if (status != ARCHON_COST_OK)
            return status;
        status = archon_cost_weighted_score_long(right, profile, &right_score);
        if (status != ARCHON_COST_OK)
            return status;
        *relation = left_score < right_score ? ARCHON_COST_BETTER :
            (left_score > right_score ? ARCHON_COST_WORSE : ARCHON_COST_EQUAL);
        return ARCHON_COST_OK;
    }
    {
        size_t index;
        for (index = 0u; index < profile->order_count; ++index) {
            const ArchonCostValue *a =
                archon_cost_value(left, profile->order[index]);
            const ArchonCostValue *b =
                archon_cost_value(right, profile->order[index]);
            if (a->known == 0u || b->known == 0u)
                return ARCHON_COST_UNAVAILABLE;
            if (a->value < b->value) {
                *relation = ARCHON_COST_BETTER;
                return ARCHON_COST_OK;
            }
            if (a->value > b->value) {
                *relation = ARCHON_COST_WORSE;
                return ARCHON_COST_OK;
            }
        }
    }
    *relation = ARCHON_COST_EQUAL;
    return ARCHON_COST_OK;
}

ArchonCostStatus archon_cost_select_best(
        const ArchonCost *costs, size_t count, const ArchonCostProfile *profile,
        size_t *selected) {
    size_t best;
    size_t index;
    ArchonCostStatus status;
    if (costs == NULL || count == 0u || profile == NULL || selected == NULL)
        return ARCHON_COST_INVALID_ARGUMENT;
    *selected = SIZE_MAX;
    status = archon_cost_validate_profile(profile);
    if (status != ARCHON_COST_OK)
        return status;
    best = 0u;
    for (index = 1u; index < count; ++index) {
        ArchonCostRelation relation;
        status = archon_cost_compare(&costs[index], &costs[best], profile, &relation);
        if (status != ARCHON_COST_OK)
            return status;
        if (relation == ARCHON_COST_BETTER)
            best = index;
    }
    *selected = best;
    return ARCHON_COST_OK;
}

size_t archon_cost_profile_count(void) {
    return archon_cost_profile_table_count;
}

const ArchonCostProfile *archon_cost_profile(size_t index) {
    if (index >= archon_cost_profile_table_count)
        return NULL;
    return &archon_cost_profiles[index];
}

const ArchonCostProfile *archon_cost_profile_by_name(const char *name) {
    size_t index;
    if (name == NULL)
        return NULL;
    for (index = 0u; index < archon_cost_profile_table_count; ++index)
        if (strcmp(name, archon_cost_profiles[index].name) == 0)
            return &archon_cost_profiles[index];
    if (strcmp(name, "oz") == 0)
        return archon_cost_profile_by_name("Oz");
    if (strcmp(name, "o3") == 0)
        return archon_cost_profile_by_name("O3");
    return NULL;
}

const ArchonCostProfile *archon_cost_select_profile(const char *name) {
    return archon_cost_profile_by_name(name);
}

const ArchonCostProfile *archon_cost_default_profile(void) {
    const ArchonCostProfile *profile =
        archon_cost_profile_by_name(archon_cost_default_profile_name);
    return profile != NULL ? profile : &archon_cost_profiles[0];
}

size_t archon_cost_architecture_count(void) {
    return archon_cost_architecture_table_count;
}

const ArchonCostArchitecture *archon_cost_architecture(size_t index) {
    if (index >= archon_cost_architecture_table_count)
        return NULL;
    return &archon_cost_architectures[index];
}

size_t archon_cost_instruction_count(void) {
    return archon_cost_instruction_table_count;
}

const ArchonInstructionCost *archon_cost_instruction(size_t index) {
    if (index >= archon_cost_instruction_table_count)
        return NULL;
    return &archon_cost_instructions[index];
}

const ArchonInstructionCost *archon_cost_find_instruction(
        const char *architecture, const char *name) {
    size_t index;
    if (architecture == NULL || name == NULL)
        return NULL;
    for (index = 0u; index < archon_cost_instruction_table_count; ++index) {
        const ArchonInstructionCost *instruction = &archon_cost_instructions[index];
        if (strcmp(architecture, instruction->architecture) == 0 &&
                strcmp(name, instruction->name) == 0)
            return instruction;
    }
    return NULL;
}

const ArchonInstructionCost *archon_cost_instruction_by_name(
        const char *architecture, const char *name) {
    return archon_cost_find_instruction(architecture, name);
}

size_t archon_cost_state_effect_count(void) {
    return archon_cost_effect_table_count;
}

const ArchonCostStateEffect *archon_cost_state_effect(size_t index) {
    if (index >= archon_cost_effect_table_count)
        return NULL;
    return &archon_cost_effects[index];
}

const ArchonCostStateEffect *archon_cost_instruction_effect(
        const ArchonInstructionCost *instruction, size_t index) {
    if (instruction == NULL || index >= instruction->effect_count ||
            instruction->first_effect > archon_cost_effect_table_count ||
            index >= archon_cost_effect_table_count - instruction->first_effect)
        return NULL;
    return archon_cost_state_effect(instruction->first_effect + index);
}
"""
