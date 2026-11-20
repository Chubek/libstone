"""Generate validated, representation-neutral target facts for IR backends.

The current ArchDB schema describes architectural encodings and semantics, but
does not describe an ABI or a microarchitecture.  This adapter deliberately
keeps that boundary visible: fixed instruction size is known, while latency,
throughput, micro-op counts, execution resources, ABI details, and semantic
equivalences are emitted as unknown.  Consumers therefore cannot accidentally
treat an invented scheduling value as an architecture fact.
"""

from __future__ import annotations

from collections import defaultdict
from collections.abc import Mapping, Sequence
import hashlib
import json
import re

from .. import database
from ..diagnostics import ArchonError


STAGE_ORDER = 100
SCHEMA_VERSION = 1

_NONE_U32 = 0xFFFFFFFF
_NONE_INDEX = -1
_IDENTIFIER_LIMIT = 48
_C_KEYWORDS = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "inline", "int", "long", "register", "restrict", "return", "short",
    "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
    "unsigned", "void", "volatile", "while", "_Bool", "_Complex", "_Imaginary",
}
_DIRECTIONS = {"in": 1, "out": 2, "inout": 3}
_OPERAND_KINDS = {"reg": 1, "imm": 2}
_TYPE_KINDS = {"bits": 1, "uint": 2, "sint": 3}
_RESOURCE_KINDS = {"scalar": 1, "stack": 2, "memory": 3}
_EFFECT_KEYS = (
    ("reads", "read-register", 1),
    ("writes", "write-register", 2),
    ("state_reads", "read-scalar", 3),
    ("state_writes", "write-scalar", 4),
    ("stack_reads", "read-stack", 5),
    ("stack_writes", "write-stack", 6),
    ("memory_reads", "read-memory", 7),
    ("memory_writes", "write-memory", 8),
)
_COST_KINDS = (
    ("code-size-bytes", 1),
    ("latency-cycles", 2),
    ("reciprocal-throughput-cycles", 3),
    ("micro-ops", 4),
    ("energy-picojoules", 5),
)


def _canonical_json(value: object) -> str:
    try:
        return json.dumps(
            value, ensure_ascii=True, sort_keys=True, separators=(",", ":"),
            allow_nan=False,
        )
    except (TypeError, ValueError, RecursionError) as exc:
        raise ArchonError(f"target facts require JSON-compatible input: {exc}") from exc


def _fingerprint(db: dict) -> str:
    return hashlib.sha256(_canonical_json(db).encode("ascii")).hexdigest()


def _check_text(value: object, path: str) -> str:
    if not isinstance(value, str):
        raise ArchonError(f"invalid target facts: {path} must be a string")
    if "\0" in value:
        raise ArchonError(f"invalid target facts: {path} contains a NUL byte")
    return value


def _base_identifier(name: str) -> str:
    """Return a portable filename/C-identifier component.

    A short name digest is attached whenever sanitising loses information.
    Thus names such as ``a-b`` and ``a_b`` remain distinct even when generated
    in separate invocations, not merely when they happen to be in one context.
    """

    pieces = []
    lossy = False
    previous_underscore = False
    for character in name:
        if character.isascii() and (character.isalnum() or character == "_"):
            pieces.append(character)
            previous_underscore = character == "_"
        else:
            lossy = True
            if not previous_underscore:
                pieces.append("_")
                previous_underscore = True
    value = "".join(pieces).strip("_")
    if not value:
        value = "Architecture"
        lossy = True
    if value[0].isdigit():
        value = "A_" + value
        lossy = True
    if value in _C_KEYWORDS:
        value = "A_" + value
        lossy = True
    if len(value) > _IDENTIFIER_LIMIT:
        value = value[:_IDENTIFIER_LIMIT].rstrip("_") or "Architecture"
        lossy = True
    if lossy:
        digest = hashlib.sha256(name.encode("utf-8")).hexdigest()[:12]
        value = f"{value}_{digest}"
    return value


def _context_databases(context: object) -> list[dict]:
    """Validate and copy one ArchDB, a sequence, or a pipeline context."""

    raw: object
    if isinstance(context, Mapping):
        if {"version", "architecture", "instructions"} <= set(context):
            raw = [context]
        elif "architectures" in context:
            raw = context["architectures"]
        else:
            raise ArchonError("target-facts context is missing architectures")
    elif isinstance(context, Sequence) and not isinstance(context, (str, bytes, bytearray)):
        raw = context
    else:
        raise ArchonError(
            "target-facts context must be an ArchDB, an architecture sequence, "
            "or a pipeline context"
        )
    if isinstance(raw, Mapping):
        if {"version", "architecture", "instructions"} <= set(raw):
            raw = [raw]
        else:
            raw = list(raw.values())
    if not isinstance(raw, Sequence) or isinstance(raw, (str, bytes, bytearray)):
        raise ArchonError("target-facts architectures must be a sequence of ArchDB objects")

    result = []
    for index, value in enumerate(raw):
        if not isinstance(value, dict):
            raise ArchonError(
                f"target-facts architectures[{index}] must be an ArchDB dictionary"
            )
        # Use the public database boundary, rather than depending on a caller
        # having gone through irgen.core.build_context.
        result.append(database.loads(database.dumps(value)))
    return result


def _validate_for_adapter(db: dict, db_index: int) -> None:
    architecture = _check_text(db["architecture"], f"architectures[{db_index}].architecture")
    prefix = f"architecture {architecture!r}"
    registers = db["registers"]
    if registers is not None:
        _check_text(registers["name"], f"{prefix}.registers.name")
        _check_text(registers["prefix"], f"{prefix}.registers.prefix")
        for alias in registers["aliases"]:
            _check_text(alias, f"{prefix}.registers.aliases")

    state_names = set()
    for group in ("scalars", "stacks", "memories"):
        for row_index, row in enumerate(db["state"][group]):
            name = _check_text(row["name"], f"{prefix}.state.{group}[{row_index}].name")
            if name in state_names:
                raise ArchonError(
                    f"invalid target facts: duplicate state resource {name!r} in {prefix}"
                )
            state_names.add(name)

    instruction_ids = set()
    instruction_names = set()
    for instruction_index, instruction in enumerate(db["instructions"]):
        path = f"{prefix}.instructions[{instruction_index}]"
        stable_id = _check_text(instruction["id"], f"{path}.id")
        name = _check_text(instruction["name"], f"{path}.name")
        _check_text(instruction["assembly"], f"{path}.assembly")
        if stable_id in instruction_ids:
            raise ArchonError(
                f"invalid target facts: duplicate instruction id {stable_id!r} in {prefix}"
            )
        if name in instruction_names:
            raise ArchonError(
                f"invalid target facts: duplicate instruction name {name!r} in {prefix}"
            )
        instruction_ids.add(stable_id)
        instruction_names.add(name)
        width = instruction["width"]
        mask = instruction["mask"]
        value = instruction["value"]
        if value & ~mask:
            raise ArchonError(
                f"invalid target facts: {path}.value contains bits outside its mask"
            )

        operands = {}
        for operand_index, operand in enumerate(instruction["operands"]):
            operand_path = f"{path}.operands[{operand_index}]"
            operand_name = _check_text(operand["name"], f"{operand_path}.name")
            if operand_name in operands:
                raise ArchonError(
                    f"invalid target facts: duplicate operand {operand_name!r} at {path}"
                )
            direction = operand.get("direction")
            if direction not in _DIRECTIONS:
                raise ArchonError(
                    f"invalid target facts: {operand_path}.direction is invalid"
                )
            if operand["kind"] == "reg":
                if registers is None:
                    raise ArchonError(
                        f"invalid target facts: register operand at {operand_path} "
                        "on a registerless target"
                    )
                if operand["signed"]:
                    raise ArchonError(
                        f"invalid target facts: {operand_path} register encoding is signed"
                    )
                if registers["count"] - 1 >= (1 << operand["width"]):
                    raise ArchonError(
                        f"invalid target facts: {operand_path} cannot encode the register file"
                    )
            elif direction != "in":
                raise ArchonError(
                    f"invalid target facts: immediate {operand_path} must be an input"
                )
            operands[operand_name] = (operand_index, operand)

        covered_operands = set()
        covered_bits = 0
        for field_index, field in enumerate(instruction["fields"]):
            field_path = f"{path}.fields[{field_index}]"
            operand_name = field["operand"]
            if operand_name not in operands:
                raise ArchonError(
                    f"invalid target facts: {field_path} names unknown operand "
                    f"{operand_name!r}"
                )
            if operand_name in covered_operands:
                raise ArchonError(
                    f"invalid target facts: operand {operand_name!r} has multiple fields at {path}"
                )
            lsb = field["lsb"]
            field_width = field["width"]
            if lsb < 0 or lsb + field_width > width:
                raise ArchonError(
                    f"invalid target facts: {field_path} is outside the instruction encoding"
                )
            bits = ((1 << field_width) - 1) << lsb
            if covered_bits & bits:
                raise ArchonError(
                    f"invalid target facts: overlapping operand fields at {path}"
                )
            if field_width != operands[operand_name][1]["width"]:
                raise ArchonError(
                    f"invalid target facts: {field_path} width differs from its operand"
                )
            covered_bits |= bits
            covered_operands.add(operand_name)
        if covered_operands != set(operands):
            missing = ", ".join(sorted(set(operands) - covered_operands))
            raise ArchonError(
                f"invalid target facts: unencoded operand(s) at {path}: {missing}"
            )
        if covered_bits & mask:
            raise ArchonError(
                f"invalid target facts: fixed and operand encoding bits overlap at {path}"
            )
        if (covered_bits | mask) != (1 << width) - 1:
            raise ArchonError(
                f"invalid target facts: uncovered encoding bits at {path}"
            )


def _assign_identifiers(databases: list[dict]) -> list[tuple[str, str]]:
    records = [
        {
            "index": index,
            "base": _base_identifier(db["architecture"]),
            "fingerprint": _fingerprint(db),
        }
        for index, db in enumerate(databases)
    ]
    groups: dict[str, list[dict]] = defaultdict(list)
    for record in records:
        # Avoid output collisions on case-insensitive filesystems as well.
        groups[record["base"].casefold()].append(record)

    proposals = {}
    for group in groups.values():
        if len(group) == 1:
            proposals[group[0]["index"]] = group[0]["base"]
            continue
        fingerprints: dict[str, list[dict]] = defaultdict(list)
        for record in group:
            fingerprints[record["fingerprint"]].append(record)
        for fingerprint, duplicates in sorted(fingerprints.items()):
            stem = f"{duplicates[0]['base']}_{fingerprint[:16].upper()}"
            if len(duplicates) == 1:
                proposals[duplicates[0]["index"]] = stem
            else:
                for occurrence, record in enumerate(
                    sorted(duplicates, key=lambda row: row["index"]), 1
                ):
                    proposals[record["index"]] = f"{stem}_{occurrence}"

    used = set()
    result = []
    for record in records:
        candidate = proposals[record["index"]]
        unique = candidate
        suffix = 2
        while unique.casefold() in used:
            unique = f"{candidate}_{suffix}"
            suffix += 1
        used.add(unique.casefold())
        result.append((unique, record["fingerprint"]))
    return result


def _type_row(row: dict) -> tuple[str, int]:
    return row["kind"], row["width"]


def _normalise_architecture(
    db: dict, target_index: int, identifier: str, fingerprint: str
) -> dict:
    registers = db["registers"]
    register_rows = []
    alias_rows = []
    if registers is not None:
        for index in range(registers["count"]):
            register_rows.append(
                {
                    "index": index,
                    "name": f"{registers['prefix']}{index}",
                    "zero": index == registers["zero"],
                }
            )
        alias_rows = [
            {"name": name, "register": index}
            for name, index in sorted(registers["aliases"].items())
        ]

    resource_rows = []
    for row in sorted(db["state"]["scalars"], key=lambda item: item["name"]):
        kind, width = _type_row(row["type"])
        resource_rows.append(
            {
                "kind": "scalar", "name": row["name"], "type": kind,
                "width": width, "extent": 1,
            }
        )
    for row in sorted(db["state"]["stacks"], key=lambda item: item["name"]):
        kind, width = _type_row(row["element_type"])
        resource_rows.append(
            {
                "kind": "stack", "name": row["name"], "type": kind,
                "width": width, "extent": row["capacity"],
            }
        )
    for row in sorted(db["state"]["memories"], key=lambda item: item["name"]):
        resource_rows.append(
            {
                "kind": "memory", "name": row["name"], "type": "none",
                "width": 0, "extent": row["size"],
            }
        )
    resource_lookup = {
        (row["kind"], row["name"]): index for index, row in enumerate(resource_rows)
    }

    instructions = sorted(
        db["instructions"],
        key=lambda row: (
            row["id"], row["name"], row["width"], row["mask"], row["value"],
        ),
    )
    class_keys = set()
    for instruction in instructions:
        for operand in instruction["operands"]:
            value_width = (
                registers["width"] if operand["kind"] == "reg" else operand["width"]
            )
            class_keys.add(
                (
                    _OPERAND_KINDS[operand["kind"]],
                    _DIRECTIONS[operand["direction"]],
                    value_width,
                    operand["width"],
                    bool(operand["signed"]),
                    operand["kind"],
                    operand["direction"],
                )
            )
    sorted_class_keys = sorted(class_keys)
    class_rows = []
    class_lookup = {}
    for local_id, key in enumerate(sorted_class_keys):
        _, _, value_width, encoding_width, signed, kind, direction = key
        class_lookup[(kind, direction, value_width, encoding_width, signed)] = local_id
        class_rows.append(
            {
                "local_id": local_id,
                "kind": kind,
                "direction": direction,
                "value_type": (
                    "bits" if kind == "reg" else ("sint" if signed else "uint")
                ),
                "value_width": value_width,
                "encoding_width": encoding_width,
                "signed": signed,
            }
        )

    instruction_rows = []
    for local_id, instruction in enumerate(instructions):
        operand_rows = []
        operand_positions = {}
        for position, operand in enumerate(instruction["operands"]):
            value_width = (
                registers["width"] if operand["kind"] == "reg" else operand["width"]
            )
            key = (
                operand["kind"], operand["direction"], value_width,
                operand["width"], bool(operand["signed"]),
            )
            operand_rows.append(
                {
                    "position": position,
                    "name": operand["name"],
                    "class_local_id": class_lookup[key],
                }
            )
            operand_positions[operand["name"]] = position
        field_rows = [
            {
                "operand_position": operand_positions[field["operand"]],
                "lsb": field["lsb"],
                "width": field["width"],
            }
            for field in sorted(
                instruction["fields"],
                key=lambda row: (row["lsb"], row["width"], row["operand"]),
            )
        ]

        semantics = instruction["semantics"]
        effect_rows = []
        if semantics is not None:
            effects = semantics["effects"]
            for key, kind, _ in _EFFECT_KEYS:
                for resource in sorted(effects.get(key, [])):
                    row = {
                        "kind": kind,
                        "resource": resource,
                        "operand_position": _NONE_U32,
                        "resource_local_id": _NONE_INDEX,
                    }
                    if key in {"reads", "writes"}:
                        row["operand_position"] = operand_positions[resource]
                    else:
                        resource_kind = {
                            "state_reads": "scalar",
                            "state_writes": "scalar",
                            "stack_reads": "stack",
                            "stack_writes": "stack",
                            "memory_reads": "memory",
                            "memory_writes": "memory",
                        }[key]
                        row["resource_local_id"] = resource_lookup[
                            (resource_kind, resource)
                        ]
                    effect_rows.append(row)

        size = instruction["width"] // 8
        cost_rows = []
        for cost_name, _ in _COST_KINDS:
            known = cost_name == "code-size-bytes"
            cost_rows.append(
                {
                    "kind": cost_name,
                    "known": known,
                    "numerator": size if known else 0,
                    "denominator": 1,
                }
            )
        digits = instruction["width"] // 4
        instruction_rows.append(
            {
                "local_id": local_id,
                "id": instruction["id"],
                "name": instruction["name"],
                "assembly": instruction["assembly"],
                "encoding_width": instruction["width"],
                "size": size,
                "mask": f"0x{instruction['mask']:0{digits}x}",
                "value": f"0x{instruction['value']:0{digits}x}",
                "semantics_known": semantics is not None,
                "effects_known": semantics is not None,
                # ArchSem describes behavior but does not assert equivalence
                # with another instruction or IR operation.
                "equivalence_known": False,
                "semantics_json": (
                    _canonical_json(semantics) if semantics is not None else None
                ),
                "operands": operand_rows,
                "fields": field_rows,
                "effects": effect_rows,
                "costs": cost_rows,
                "schedule": {
                    "policy": "conservative-unknown",
                    "latency_known": False,
                    "throughput_known": False,
                    "micro_ops_known": False,
                    "resources_known": False,
                    "conservative_barrier": True,
                    "may_reorder": False,
                    "may_speculate": False,
                },
            }
        )

    abi_known = "abi" in db
    return {
        "index": target_index,
        "identifier": identifier,
        "name": db["architecture"],
        "fingerprint": fingerprint,
        "configuration_json": _canonical_json(
            {"features": db["features"], "parameters": db["parameters"]}
        ),
        "address_width": db["address_width"],
        "endian": db["endian"],
        "abi_known": abi_known,
        "abi_json": _canonical_json(db["abi"]) if abi_known else None,
        "register_file": registers is not None,
        "register_file_name": registers["name"] if registers is not None else "",
        "register_prefix": registers["prefix"] if registers is not None else "",
        "register_width": registers["width"] if registers is not None else 0,
        "register_count": registers["count"] if registers is not None else 0,
        "zero_present": registers is not None and registers["zero"] is not None,
        "zero_index": (
            registers["zero"]
            if registers is not None and registers["zero"] is not None
            else _NONE_U32
        ),
        "registers": register_rows,
        "aliases": alias_rows,
        "resources": resource_rows,
        "classes": class_rows,
        "instructions": instruction_rows,
    }


def normalize(context: object) -> tuple[dict, ...]:
    """Return deterministic normalized facts for one or more ArchDB objects.

    The returned dictionaries are fresh data and are useful to non-C pipeline
    integrations.  Callers should treat their shape as the Python counterpart
    of ``TargetFacts.h``.
    """

    databases = _context_databases(context)
    for index, db in enumerate(databases):
        _validate_for_adapter(db, index)
    identities = _assign_identifiers(databases)
    return tuple(
        _normalise_architecture(db, index, *identities[index])
        for index, db in enumerate(databases)
    )


def _flatten(architectures: tuple[dict, ...]) -> dict:
    tables = {
        "architectures": [],
        "registers": [],
        "aliases": [],
        "resources": [],
        "classes": [],
        "instructions": [],
        "operands": [],
        "fields": [],
        "effects": [],
        "costs": [],
        "schedules": [],
    }
    for architecture in architectures:
        target = architecture["index"]
        register_first = len(tables["registers"])
        for row in architecture["registers"]:
            tables["registers"].append({**row, "target": target})
        alias_first = len(tables["aliases"])
        for row in architecture["aliases"]:
            tables["aliases"].append({**row, "target": target})
        resource_first = len(tables["resources"])
        for local_id, row in enumerate(architecture["resources"]):
            tables["resources"].append(
                {**row, "target": target, "local_id": local_id}
            )
        class_first = len(tables["classes"])
        for row in architecture["classes"]:
            tables["classes"].append({**row, "target": target})
        instruction_first = len(tables["instructions"])

        for instruction in architecture["instructions"]:
            instruction_index = len(tables["instructions"])
            operand_first = len(tables["operands"])
            for row in instruction["operands"]:
                tables["operands"].append(
                    {
                        **row,
                        "instruction": instruction_index,
                        "class_index": class_first + row["class_local_id"],
                    }
                )
            field_first = len(tables["fields"])
            for row in instruction["fields"]:
                tables["fields"].append(
                    {
                        **row,
                        "instruction": instruction_index,
                        "operand_index": operand_first + row["operand_position"],
                    }
                )
            effect_first = len(tables["effects"])
            for row in instruction["effects"]:
                resource_index = (
                    resource_first + row["resource_local_id"]
                    if row["resource_local_id"] != _NONE_INDEX
                    else _NONE_INDEX
                )
                tables["effects"].append(
                    {
                        **row,
                        "instruction": instruction_index,
                        "resource_index": resource_index,
                    }
                )
            cost_first = len(tables["costs"])
            for row in instruction["costs"]:
                tables["costs"].append(
                    {**row, "instruction": instruction_index}
                )
            schedule_index = len(tables["schedules"])
            tables["schedules"].append(
                {**instruction["schedule"], "instruction": instruction_index}
            )
            tables["instructions"].append(
                {
                    **{
                        key: value
                        for key, value in instruction.items()
                        if key not in {
                            "operands", "fields", "effects", "costs", "schedule"
                        }
                    },
                    "index": instruction_index,
                    "target": target,
                    "operand_first": operand_first,
                    "operand_count": len(instruction["operands"]),
                    "field_first": field_first,
                    "field_count": len(instruction["fields"]),
                    "effect_first": effect_first,
                    "effect_count": len(instruction["effects"]),
                    "cost_first": cost_first,
                    "cost_count": len(instruction["costs"]),
                    "schedule_index": schedule_index,
                }
            )

        tables["architectures"].append(
            {
                **{
                    key: value
                    for key, value in architecture.items()
                    if key not in {
                        "registers", "aliases", "resources", "classes", "instructions"
                    }
                },
                "register_first": register_first,
                "register_fact_count": len(architecture["registers"]),
                "alias_first": alias_first,
                "alias_count": len(architecture["aliases"]),
                "resource_first": resource_first,
                "resource_count": len(architecture["resources"]),
                "class_first": class_first,
                "class_count": len(architecture["classes"]),
                "instruction_first": instruction_first,
                "instruction_count": len(architecture["instructions"]),
            }
        )
    return tables


def _c_string(value: object) -> str:
    pieces = ['"']
    for byte in str(value).encode("utf-8"):
        if byte == 0x22:
            pieces.append(r"\"")
        elif byte == 0x5C:
            pieces.append(r"\\")
        elif byte == 0x0A:
            pieces.append(r"\n")
        elif byte == 0x0D:
            pieces.append(r"\r")
        elif byte == 0x09:
            pieces.append(r"\t")
        # Escape '?' to prevent C trigraph replacement before tokenisation.
        elif 0x20 <= byte < 0x7F and byte != 0x3F:
            pieces.append(chr(byte))
        else:
            pieces.append(f"\\{byte:03o}")
    pieces.append('"')
    return "".join(pieces)


def _knowledge(value: bool) -> str:
    return "ARCHON_TARGET_KNOWN" if value else "ARCHON_TARGET_UNKNOWN"


def _presence(value: bool) -> str:
    return "ARCHON_TARGET_PRESENT" if value else "ARCHON_TARGET_ABSENT"


def _endian(value: str) -> str:
    return (
        "ARCHON_TARGET_ENDIAN_LITTLE"
        if value == "little"
        else "ARCHON_TARGET_ENDIAN_BIG"
    )


def _type_kind(value: str) -> str:
    return {
        "none": "ARCHON_TARGET_VALUE_NONE",
        "bits": "ARCHON_TARGET_VALUE_BITS",
        "uint": "ARCHON_TARGET_VALUE_UINT",
        "sint": "ARCHON_TARGET_VALUE_SINT",
    }[value]


def _operand_kind(value: str) -> str:
    return {
        "reg": "ARCHON_TARGET_OPERAND_REGISTER",
        "imm": "ARCHON_TARGET_OPERAND_IMMEDIATE",
    }[value]


def _direction(value: str) -> str:
    return {
        "in": "ARCHON_TARGET_DIRECTION_IN",
        "out": "ARCHON_TARGET_DIRECTION_OUT",
        "inout": "ARCHON_TARGET_DIRECTION_INOUT",
    }[value]


def _resource_kind(value: str) -> str:
    return {
        "scalar": "ARCHON_TARGET_RESOURCE_SCALAR",
        "stack": "ARCHON_TARGET_RESOURCE_STACK",
        "memory": "ARCHON_TARGET_RESOURCE_MEMORY",
    }[value]


def _effect_kind(value: str) -> str:
    return {
        "read-register": "ARCHON_TARGET_EFFECT_READ_REGISTER",
        "write-register": "ARCHON_TARGET_EFFECT_WRITE_REGISTER",
        "read-scalar": "ARCHON_TARGET_EFFECT_READ_SCALAR",
        "write-scalar": "ARCHON_TARGET_EFFECT_WRITE_SCALAR",
        "read-stack": "ARCHON_TARGET_EFFECT_READ_STACK",
        "write-stack": "ARCHON_TARGET_EFFECT_WRITE_STACK",
        "read-memory": "ARCHON_TARGET_EFFECT_READ_MEMORY",
        "write-memory": "ARCHON_TARGET_EFFECT_WRITE_MEMORY",
    }[value]


def _cost_kind(value: str) -> str:
    return {
        "code-size-bytes": "ARCHON_TARGET_COST_CODE_SIZE_BYTES",
        "latency-cycles": "ARCHON_TARGET_COST_LATENCY_CYCLES",
        "reciprocal-throughput-cycles":
            "ARCHON_TARGET_COST_RECIPROCAL_THROUGHPUT_CYCLES",
        "micro-ops": "ARCHON_TARGET_COST_MICRO_OPS",
        "energy-picojoules": "ARCHON_TARGET_COST_ENERGY_PICOJOULES",
    }[value]


def _array(rows: list[str], zero: str) -> str:
    return "\n".join(rows if rows else [zero])


def _source(tables: dict) -> str:
    architecture_rows = []
    for row in tables["architectures"]:
        architecture_rows.append(
            "    {"
            f"{row['index']}u, {_c_string(row['identifier'])}, {_c_string(row['name'])}, "
            f"{_c_string(row['fingerprint'])}, {_c_string(row['configuration_json'])}, "
            f"{row['address_width']}u, {_endian(row['endian'])}, "
            f"{_knowledge(row['abi_known'])}, "
            f"{_c_string(row['abi_json']) if row['abi_known'] else 'NULL'}, "
            f"{_presence(row['register_file'])}, "
            f"{_c_string(row['register_file_name'])}, {_c_string(row['register_prefix'])}, "
            f"{row['register_count']}u, {row['register_width']}u, "
            f"{_presence(row['zero_present'])}, {row['zero_index']}u, "
            f"{row['register_first']}u, {row['register_fact_count']}u, "
            f"{row['alias_first']}u, {row['alias_count']}u, "
            f"{row['resource_first']}u, {row['resource_count']}u, "
            f"{row['class_first']}u, {row['class_count']}u, "
            f"{row['instruction_first']}u, {row['instruction_count']}u"
            "},"
        )
    register_rows = [
        "    {"
        f"{row['target']}u, {row['index']}u, {_c_string(row['name'])}, "
        f"{1 if row['zero'] else 0}"
        "},"
        for row in tables["registers"]
    ]
    alias_rows = [
        "    {"
        f"{row['target']}u, {_c_string(row['name'])}, {row['register']}u"
        "},"
        for row in tables["aliases"]
    ]
    resource_rows = [
        "    {"
        f"{row['target']}u, {row['local_id']}u, {_resource_kind(row['kind'])}, "
        f"{_c_string(row['name'])}, {_type_kind(row['type'])}, {row['width']}u, "
        f"UINT64_C({row['extent']})"
        "},"
        for row in tables["resources"]
    ]
    class_rows = [
        "    {"
        f"{row['target']}u, {row['local_id']}u, {_operand_kind(row['kind'])}, "
        f"{_direction(row['direction'])}, {_type_kind(row['value_type'])}, "
        f"{row['value_width']}u, {row['encoding_width']}u, "
        f"{1 if row['signed'] else 0}"
        "},"
        for row in tables["classes"]
    ]
    instruction_rows = [
        "    {"
        f"{row['index']}u, {row['target']}u, {row['local_id']}u, "
        f"{_c_string(row['id'])}, {_c_string(row['name'])}, "
        f"{_c_string(row['assembly'])}, {row['encoding_width']}u, {row['size']}u, "
        f"{_c_string(row['mask'])}, {_c_string(row['value'])}, "
        f"{_knowledge(row['semantics_known'])}, {_knowledge(row['effects_known'])}, "
        f"{_knowledge(row['equivalence_known'])}, "
        f"{_c_string(row['semantics_json']) if row['semantics_known'] else 'NULL'}, "
        f"{row['operand_first']}u, {row['operand_count']}u, "
        f"{row['field_first']}u, {row['field_count']}u, "
        f"{row['effect_first']}u, {row['effect_count']}u, "
        f"{row['cost_first']}u, {row['cost_count']}u, {row['schedule_index']}u"
        "},"
        for row in tables["instructions"]
    ]
    operand_rows = [
        "    {"
        f"{row['instruction']}u, {row['position']}u, {_c_string(row['name'])}, "
        f"{row['class_index']}u"
        "},"
        for row in tables["operands"]
    ]
    field_rows = [
        "    {"
        f"{row['instruction']}u, {row['operand_index']}u, "
        f"{row['lsb']}u, {row['width']}u"
        "},"
        for row in tables["fields"]
    ]
    effect_rows = [
        "    {"
        f"{row['instruction']}u, {_effect_kind(row['kind'])}, "
        f"{_c_string(row['resource'])}, {row['operand_position']}u, "
        f"{'(size_t)-1' if row['resource_index'] == _NONE_INDEX else str(row['resource_index']) + 'u'}"
        "},"
        for row in tables["effects"]
    ]
    cost_rows = [
        "    {"
        f"{row['instruction']}u, {_cost_kind(row['kind'])}, "
        f"{_knowledge(row['known'])}, UINT64_C({row['numerator']}), "
        f"UINT64_C({row['denominator']})"
        "},"
        for row in tables["costs"]
    ]
    schedule_rows = [
        "    {"
        f"{row['instruction']}u, ARCHON_TARGET_SCHEDULE_CONSERVATIVE_UNKNOWN, "
        f"{_knowledge(row['latency_known'])}, "
        f"{_knowledge(row['throughput_known'])}, "
        f"{_knowledge(row['micro_ops_known'])}, "
        f"{_knowledge(row['resources_known'])}, "
        f"{1 if row['conservative_barrier'] else 0}, "
        f"{1 if row['may_reorder'] else 0}, "
        f"{1 if row['may_speculate'] else 0}"
        "},"
        for row in tables["schedules"]
    ]

    replacements = {
        "@ARCHITECTURES@": _array(architecture_rows, "    {0},"),
        "@ARCHITECTURE_COUNT@": str(len(architecture_rows)),
        "@REGISTERS@": _array(register_rows, "    {0},"),
        "@REGISTER_COUNT@": str(len(register_rows)),
        "@ALIASES@": _array(alias_rows, "    {0},"),
        "@ALIAS_COUNT@": str(len(alias_rows)),
        "@RESOURCES@": _array(resource_rows, "    {0},"),
        "@RESOURCE_COUNT@": str(len(resource_rows)),
        "@CLASSES@": _array(class_rows, "    {0},"),
        "@CLASS_COUNT@": str(len(class_rows)),
        "@INSTRUCTIONS@": _array(instruction_rows, "    {0},"),
        "@INSTRUCTION_COUNT@": str(len(instruction_rows)),
        "@OPERANDS@": _array(operand_rows, "    {0},"),
        "@OPERAND_COUNT@": str(len(operand_rows)),
        "@FIELDS@": _array(field_rows, "    {0},"),
        "@FIELD_COUNT@": str(len(field_rows)),
        "@EFFECTS@": _array(effect_rows, "    {0},"),
        "@EFFECT_COUNT@": str(len(effect_rows)),
        "@COSTS@": _array(cost_rows, "    {0},"),
        "@COST_COUNT@": str(len(cost_rows)),
        "@SCHEDULES@": _array(schedule_rows, "    {0},"),
        "@SCHEDULE_COUNT@": str(len(schedule_rows)),
    }
    result = _SOURCE
    for token, value in replacements.items():
        result = result.replace(token, value)
    if re.search(r"@[A-Z_]+@", result):
        raise AssertionError("unexpanded TargetFacts.c template token")
    return result


_MACROS = (
    (
        "ARCHON_TARGET_BEGIN",
        "SCHEMA, IDENTIFIER, NAME, FINGERPRINT, CONFIGURATION, ADDRESS_WIDTH, "
        "ENDIAN, ABI_KNOWLEDGE, ABI_JSON, REGISTER_PRESENCE, INSTRUCTION_COUNT, "
        "STATE_RESOURCE_COUNT, OPERAND_CLASS_COUNT",
    ),
    (
        "ARCHON_TARGET_REGISTER_FILE",
        "PRESENCE, NAME, PREFIX, COUNT, WIDTH, ZERO_PRESENCE, ZERO_INDEX",
    ),
    ("ARCHON_TARGET_REGISTER", "INDEX, NAME, IS_ZERO"),
    ("ARCHON_TARGET_ALIAS", "NAME, REGISTER_INDEX"),
    (
        "ARCHON_TARGET_STATE_RESOURCE",
        "LOCAL_ID, KIND, NAME, VALUE_KIND, WIDTH, EXTENT",
    ),
    (
        "ARCHON_TARGET_OPERAND_CLASS",
        "LOCAL_ID, KIND, DIRECTION, VALUE_KIND, VALUE_WIDTH, ENCODING_WIDTH, "
        "IS_SIGNED",
    ),
    (
        "ARCHON_TARGET_INSTRUCTION",
        "LOCAL_ID, STABLE_ID, NAME, ASSEMBLY, ENCODING_WIDTH, SIZE_BYTES, "
        "MASK_HEX, VALUE_HEX, SEMANTICS_KNOWLEDGE, EFFECTS_KNOWLEDGE, "
        "EQUIVALENCE_KNOWLEDGE",
    ),
    (
        "ARCHON_TARGET_OPERAND",
        "INSTRUCTION_ID, POSITION, NAME, OPERAND_CLASS_ID",
    ),
    (
        "ARCHON_TARGET_ENCODING_FIELD",
        "INSTRUCTION_ID, OPERAND_POSITION, LSB, WIDTH",
    ),
    ("ARCHON_TARGET_SEMANTICS", "INSTRUCTION_ID, SEMANTICS_JSON"),
    (
        "ARCHON_TARGET_EFFECT",
        "INSTRUCTION_ID, KIND, RESOURCE, OPERAND_POSITION, STATE_RESOURCE_ID",
    ),
    (
        "ARCHON_TARGET_COST",
        "INSTRUCTION_ID, KIND, KNOWLEDGE, NUMERATOR, DENOMINATOR",
    ),
    (
        "ARCHON_TARGET_SCHEDULE",
        "INSTRUCTION_ID, POLICY, LATENCY_KNOWLEDGE, THROUGHPUT_KNOWLEDGE, "
        "MICRO_OPS_KNOWLEDGE, RESOURCE_KNOWLEDGE, CONSERVATIVE_BARRIER, "
        "MAY_REORDER, MAY_SPECULATE",
    ),
    ("ARCHON_TARGET_END", ""),
)


def _macro_file(architecture: dict) -> str:
    lines = [
        "/* Generated by Archon target facts. Repeat-includable X-macro data. */",
    ]
    for index, (name, arguments) in enumerate(_MACROS):
        marker = f"ARCHON_TARGET_FACTS_PRIVATE_{index}"
        lines.extend(
            [
                f"#ifndef {name}",
                f"# define {name}({arguments})" if arguments else f"# define {name}()",
                f"# define {marker}",
                "#endif",
            ]
        )
    lines.extend(
        [
            "",
            "ARCHON_TARGET_BEGIN("
            f"{SCHEMA_VERSION}u, {_c_string(architecture['identifier'])}, "
            f"{_c_string(architecture['name'])}, "
            f"{_c_string(architecture['fingerprint'])}, "
            f"{_c_string(architecture['configuration_json'])}, "
            f"{architecture['address_width']}u, {_endian(architecture['endian'])}, "
            f"{_knowledge(architecture['abi_known'])}, "
            f"{_c_string(architecture['abi_json']) if architecture['abi_known'] else _c_string('')}, "
            f"{_presence(architecture['register_file'])}, "
            f"{len(architecture['instructions'])}u, "
            f"{len(architecture['resources'])}u, {len(architecture['classes'])}u)",
            "ARCHON_TARGET_REGISTER_FILE("
            f"{_presence(architecture['register_file'])}, "
            f"{_c_string(architecture['register_file_name'])}, "
            f"{_c_string(architecture['register_prefix'])}, "
            f"{architecture['register_count']}u, {architecture['register_width']}u, "
            f"{_presence(architecture['zero_present'])}, "
            f"{architecture['zero_index']}u)",
        ]
    )
    for row in architecture["registers"]:
        lines.append(
            "ARCHON_TARGET_REGISTER("
            f"{row['index']}u, {_c_string(row['name'])}, {1 if row['zero'] else 0})"
        )
    for row in architecture["aliases"]:
        lines.append(
            "ARCHON_TARGET_ALIAS("
            f"{_c_string(row['name'])}, {row['register']}u)"
        )
    for local_id, row in enumerate(architecture["resources"]):
        lines.append(
            "ARCHON_TARGET_STATE_RESOURCE("
            f"{local_id}u, {_resource_kind(row['kind'])}, {_c_string(row['name'])}, "
            f"{_type_kind(row['type'])}, {row['width']}u, "
            f"UINT64_C({row['extent']}))"
        )
    for row in architecture["classes"]:
        lines.append(
            "ARCHON_TARGET_OPERAND_CLASS("
            f"{row['local_id']}u, {_operand_kind(row['kind'])}, "
            f"{_direction(row['direction'])}, {_type_kind(row['value_type'])}, "
            f"{row['value_width']}u, {row['encoding_width']}u, "
            f"{1 if row['signed'] else 0})"
        )
    for instruction in architecture["instructions"]:
        local_id = instruction["local_id"]
        lines.append(
            "ARCHON_TARGET_INSTRUCTION("
            f"{local_id}u, {_c_string(instruction['id'])}, "
            f"{_c_string(instruction['name'])}, {_c_string(instruction['assembly'])}, "
            f"{instruction['encoding_width']}u, {instruction['size']}u, "
            f"{_c_string(instruction['mask'])}, {_c_string(instruction['value'])}, "
            f"{_knowledge(instruction['semantics_known'])}, "
            f"{_knowledge(instruction['effects_known'])}, "
            f"{_knowledge(instruction['equivalence_known'])})"
        )
        for operand in instruction["operands"]:
            lines.append(
                "ARCHON_TARGET_OPERAND("
                f"{local_id}u, {operand['position']}u, {_c_string(operand['name'])}, "
                f"{operand['class_local_id']}u)"
            )
        for field in instruction["fields"]:
            lines.append(
                "ARCHON_TARGET_ENCODING_FIELD("
                f"{local_id}u, {field['operand_position']}u, "
                f"{field['lsb']}u, {field['width']}u)"
            )
        if instruction["semantics_known"]:
            lines.append(
                "ARCHON_TARGET_SEMANTICS("
                f"{local_id}u, {_c_string(instruction['semantics_json'])})"
            )
        for effect in instruction["effects"]:
            resource_id = (
                f"{effect['resource_local_id']}u"
                if effect["resource_local_id"] != _NONE_INDEX
                else f"{_NONE_U32}u"
            )
            lines.append(
                "ARCHON_TARGET_EFFECT("
                f"{local_id}u, {_effect_kind(effect['kind'])}, "
                f"{_c_string(effect['resource'])}, {effect['operand_position']}u, "
                f"{resource_id})"
            )
        for cost in instruction["costs"]:
            lines.append(
                "ARCHON_TARGET_COST("
                f"{local_id}u, {_cost_kind(cost['kind'])}, "
                f"{_knowledge(cost['known'])}, UINT64_C({cost['numerator']}), "
                f"UINT64_C({cost['denominator']}))"
            )
        schedule = instruction["schedule"]
        lines.append(
            "ARCHON_TARGET_SCHEDULE("
            f"{local_id}u, ARCHON_TARGET_SCHEDULE_CONSERVATIVE_UNKNOWN, "
            f"{_knowledge(schedule['latency_known'])}, "
            f"{_knowledge(schedule['throughput_known'])}, "
            f"{_knowledge(schedule['micro_ops_known'])}, "
            f"{_knowledge(schedule['resources_known'])}, "
            f"{1 if schedule['conservative_barrier'] else 0}, "
            f"{1 if schedule['may_reorder'] else 0}, "
            f"{1 if schedule['may_speculate'] else 0})"
        )
    lines.extend(["ARCHON_TARGET_END()", ""])
    for index, (name, _) in reversed(tuple(enumerate(_MACROS))):
        marker = f"ARCHON_TARGET_FACTS_PRIVATE_{index}"
        lines.extend(
            [
                f"#ifdef {marker}",
                f"# undef {name}",
                f"# undef {marker}",
                "#endif",
            ]
        )
    return "\n".join(lines) + "\n"


def emit(context: object) -> dict[str, str]:
    """Emit the common C99 fact database and one X-macro file per target."""

    architectures = normalize(context)
    tables = _flatten(architectures)
    outputs = {
        "TargetFacts.h": _HEADER,
        "TargetFacts.c": _source(tables),
    }
    for architecture in architectures:
        name = f"Target_{architecture['identifier']}.i"
        if name in outputs:
            # _assign_identifiers should make this unreachable; keep the output
            # boundary defensive because duplicate paths are destructive.
            raise ArchonError(f"target-facts output collision for {name!r}")
        outputs[name] = _macro_file(architecture)
    return dict(sorted(outputs.items()))


_HEADER = r"""/* Generated by Archon: normalized target facts, strict C99. */
#ifndef ARCHON_GENERATED_TARGET_FACTS_H
#define ARCHON_GENERATED_TARGET_FACTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ARCHON_TARGET_FACTS_SCHEMA_VERSION 1u
#define ARCHON_TARGET_OPERAND_NONE UINT32_MAX
#define ARCHON_TARGET_STATE_RESOURCE_NONE ((size_t)-1)

typedef enum ArchonTargetKnowledge {
    ARCHON_TARGET_UNKNOWN = 0,
    ARCHON_TARGET_KNOWN = 1
} ArchonTargetKnowledge;

typedef enum ArchonTargetPresence {
    ARCHON_TARGET_PRESENCE_UNKNOWN = 0,
    ARCHON_TARGET_ABSENT = 1,
    ARCHON_TARGET_PRESENT = 2
} ArchonTargetPresence;

typedef enum ArchonTargetEndian {
    ARCHON_TARGET_ENDIAN_LITTLE = 1,
    ARCHON_TARGET_ENDIAN_BIG = 2
} ArchonTargetEndian;

typedef enum ArchonTargetValueKind {
    ARCHON_TARGET_VALUE_NONE = 0,
    ARCHON_TARGET_VALUE_BITS = 1,
    ARCHON_TARGET_VALUE_UINT = 2,
    ARCHON_TARGET_VALUE_SINT = 3
} ArchonTargetValueKind;

typedef enum ArchonTargetOperandKind {
    ARCHON_TARGET_OPERAND_REGISTER = 1,
    ARCHON_TARGET_OPERAND_IMMEDIATE = 2
} ArchonTargetOperandKind;

typedef enum ArchonTargetDirection {
    ARCHON_TARGET_DIRECTION_IN = 1,
    ARCHON_TARGET_DIRECTION_OUT = 2,
    ARCHON_TARGET_DIRECTION_INOUT = 3
} ArchonTargetDirection;

typedef enum ArchonTargetResourceKind {
    ARCHON_TARGET_RESOURCE_SCALAR = 1,
    ARCHON_TARGET_RESOURCE_STACK = 2,
    ARCHON_TARGET_RESOURCE_MEMORY = 3
} ArchonTargetResourceKind;

typedef enum ArchonTargetEffectKind {
    ARCHON_TARGET_EFFECT_READ_REGISTER = 1,
    ARCHON_TARGET_EFFECT_WRITE_REGISTER = 2,
    ARCHON_TARGET_EFFECT_READ_SCALAR = 3,
    ARCHON_TARGET_EFFECT_WRITE_SCALAR = 4,
    ARCHON_TARGET_EFFECT_READ_STACK = 5,
    ARCHON_TARGET_EFFECT_WRITE_STACK = 6,
    ARCHON_TARGET_EFFECT_READ_MEMORY = 7,
    ARCHON_TARGET_EFFECT_WRITE_MEMORY = 8
} ArchonTargetEffectKind;

typedef enum ArchonTargetCostKind {
    ARCHON_TARGET_COST_CODE_SIZE_BYTES = 1,
    ARCHON_TARGET_COST_LATENCY_CYCLES = 2,
    ARCHON_TARGET_COST_RECIPROCAL_THROUGHPUT_CYCLES = 3,
    ARCHON_TARGET_COST_MICRO_OPS = 4,
    ARCHON_TARGET_COST_ENERGY_PICOJOULES = 5
} ArchonTargetCostKind;

typedef enum ArchonTargetSchedulePolicy {
    /*
     * No microarchitecture was supplied. A scheduler must preserve ordering
     * and must not speculate; zero-valued metric payloads are not estimates.
     */
    ARCHON_TARGET_SCHEDULE_CONSERVATIVE_UNKNOWN = 1,
    ARCHON_TARGET_SCHEDULE_EXPLICIT = 2
} ArchonTargetSchedulePolicy;

typedef struct ArchonTargetArchitectureFact {
    size_t index;
    const char *identifier;
    const char *name;
    const char *fingerprint;
    const char *configuration_json;
    uint32_t address_width;
    ArchonTargetEndian endian;
    ArchonTargetKnowledge abi_knowledge;
    const char *abi_json;
    ArchonTargetPresence register_file;
    const char *register_file_name;
    const char *register_prefix;
    uint32_t register_count;
    uint32_t register_width;
    ArchonTargetPresence zero_register;
    uint32_t zero_register_index;
    size_t first_register;
    size_t register_fact_count;
    size_t first_alias;
    size_t alias_count;
    size_t first_state_resource;
    size_t state_resource_count;
    size_t first_operand_class;
    size_t operand_class_count;
    size_t first_instruction;
    size_t instruction_count;
} ArchonTargetArchitectureFact;

typedef struct ArchonTargetRegisterFact {
    size_t target_index;
    uint32_t index;
    const char *name;
    int is_zero;
} ArchonTargetRegisterFact;

typedef struct ArchonTargetAliasFact {
    size_t target_index;
    const char *name;
    uint32_t register_index;
} ArchonTargetAliasFact;

typedef struct ArchonTargetStateResourceFact {
    size_t target_index;
    uint32_t local_id;
    ArchonTargetResourceKind kind;
    const char *name;
    ArchonTargetValueKind value_kind;
    uint32_t width;
    uint64_t extent;
} ArchonTargetStateResourceFact;

typedef struct ArchonTargetOperandClassFact {
    size_t target_index;
    uint32_t local_id;
    ArchonTargetOperandKind kind;
    ArchonTargetDirection direction;
    ArchonTargetValueKind value_kind;
    uint32_t value_width;
    uint32_t encoding_width;
    int is_signed;
} ArchonTargetOperandClassFact;

typedef struct ArchonTargetInstructionFact {
    size_t index;
    size_t target_index;
    uint32_t local_id;
    const char *stable_id;
    const char *name;
    const char *assembly;
    uint32_t encoding_width;
    uint32_t size_bytes;
    const char *encoding_mask_hex;
    const char *encoding_value_hex;
    ArchonTargetKnowledge semantics_knowledge;
    ArchonTargetKnowledge effects_knowledge;
    ArchonTargetKnowledge semantic_equivalence_knowledge;
    const char *semantics_json;
    size_t first_operand;
    size_t operand_count;
    size_t first_encoding_field;
    size_t encoding_field_count;
    size_t first_effect;
    size_t effect_count;
    size_t first_cost;
    size_t cost_count;
    size_t schedule_index;
} ArchonTargetInstructionFact;

typedef struct ArchonTargetOperandFact {
    size_t instruction_index;
    uint32_t position;
    const char *name;
    size_t operand_class_index;
} ArchonTargetOperandFact;

typedef struct ArchonTargetEncodingFieldFact {
    size_t instruction_index;
    size_t operand_index;
    uint32_t lsb;
    uint32_t width;
} ArchonTargetEncodingFieldFact;

typedef struct ArchonTargetEffectFact {
    size_t instruction_index;
    ArchonTargetEffectKind kind;
    const char *resource;
    uint32_t operand_position;
    size_t state_resource_index;
} ArchonTargetEffectFact;

typedef struct ArchonTargetCostFact {
    size_t instruction_index;
    ArchonTargetCostKind kind;
    ArchonTargetKnowledge knowledge;
    uint64_t numerator;
    uint64_t denominator;
} ArchonTargetCostFact;

typedef struct ArchonTargetScheduleFact {
    size_t instruction_index;
    ArchonTargetSchedulePolicy policy;
    ArchonTargetKnowledge latency_knowledge;
    ArchonTargetKnowledge throughput_knowledge;
    ArchonTargetKnowledge micro_ops_knowledge;
    ArchonTargetKnowledge execution_resources_knowledge;
    int conservative_barrier;
    int may_reorder;
    int may_speculate;
} ArchonTargetScheduleFact;

extern const ArchonTargetArchitectureFact archon_target_architectures[];
extern const size_t archon_target_architecture_count;
extern const ArchonTargetRegisterFact archon_target_registers[];
extern const size_t archon_target_register_count;
extern const ArchonTargetAliasFact archon_target_aliases[];
extern const size_t archon_target_alias_count;
extern const ArchonTargetStateResourceFact archon_target_state_resources[];
extern const size_t archon_target_state_resource_count;
extern const ArchonTargetOperandClassFact archon_target_operand_classes[];
extern const size_t archon_target_operand_class_count;
extern const ArchonTargetInstructionFact archon_target_instructions[];
extern const size_t archon_target_instruction_count;
extern const ArchonTargetOperandFact archon_target_operands[];
extern const size_t archon_target_operand_count;
extern const ArchonTargetEncodingFieldFact archon_target_encoding_fields[];
extern const size_t archon_target_encoding_field_count;
extern const ArchonTargetEffectFact archon_target_effects[];
extern const size_t archon_target_effect_count;
extern const ArchonTargetCostFact archon_target_costs[];
extern const size_t archon_target_cost_count;
extern const ArchonTargetScheduleFact archon_target_schedules[];
extern const size_t archon_target_schedule_count;

const ArchonTargetArchitectureFact *archon_target_architecture(size_t index);
const ArchonTargetArchitectureFact *archon_target_find_architecture(
    const char *identifier);
const ArchonTargetInstructionFact *archon_target_instruction(size_t index);
const ArchonTargetInstructionFact *archon_target_find_instruction(
    const ArchonTargetArchitectureFact *target, const char *name);
const ArchonTargetAliasFact *archon_target_find_alias(
    const ArchonTargetArchitectureFact *target, const char *name);

/* Returns one only when every generated range and cross-reference is valid. */
int archon_target_facts_validate(void);

/*
 * Target_<identifier>.i is a repeat-includable X-macro view. It invokes:
 * ARCHON_TARGET_BEGIN, ARCHON_TARGET_REGISTER_FILE, ARCHON_TARGET_REGISTER,
 * ARCHON_TARGET_ALIAS, ARCHON_TARGET_STATE_RESOURCE,
 * ARCHON_TARGET_OPERAND_CLASS, ARCHON_TARGET_INSTRUCTION,
 * ARCHON_TARGET_OPERAND, ARCHON_TARGET_ENCODING_FIELD,
 * ARCHON_TARGET_SEMANTICS, ARCHON_TARGET_EFFECT, ARCHON_TARGET_COST,
 * ARCHON_TARGET_SCHEDULE, and ARCHON_TARGET_END.
 *
 * Any macro not supplied by a consumer is temporarily defined as a no-op.
 * Numeric enum arguments use the constants in this header.
 */

#ifdef __cplusplus
}
#endif

#endif
"""


_SOURCE = r"""/* Generated by Archon: normalized target facts, strict C99. */
#include "TargetFacts.h"

#include <string.h>

const ArchonTargetArchitectureFact archon_target_architectures[] = {
@ARCHITECTURES@
};
const size_t archon_target_architecture_count = @ARCHITECTURE_COUNT@u;

const ArchonTargetRegisterFact archon_target_registers[] = {
@REGISTERS@
};
const size_t archon_target_register_count = @REGISTER_COUNT@u;

const ArchonTargetAliasFact archon_target_aliases[] = {
@ALIASES@
};
const size_t archon_target_alias_count = @ALIAS_COUNT@u;

const ArchonTargetStateResourceFact archon_target_state_resources[] = {
@RESOURCES@
};
const size_t archon_target_state_resource_count = @RESOURCE_COUNT@u;

const ArchonTargetOperandClassFact archon_target_operand_classes[] = {
@CLASSES@
};
const size_t archon_target_operand_class_count = @CLASS_COUNT@u;

const ArchonTargetInstructionFact archon_target_instructions[] = {
@INSTRUCTIONS@
};
const size_t archon_target_instruction_count = @INSTRUCTION_COUNT@u;

const ArchonTargetOperandFact archon_target_operands[] = {
@OPERANDS@
};
const size_t archon_target_operand_count = @OPERAND_COUNT@u;

const ArchonTargetEncodingFieldFact archon_target_encoding_fields[] = {
@FIELDS@
};
const size_t archon_target_encoding_field_count = @FIELD_COUNT@u;

const ArchonTargetEffectFact archon_target_effects[] = {
@EFFECTS@
};
const size_t archon_target_effect_count = @EFFECT_COUNT@u;

const ArchonTargetCostFact archon_target_costs[] = {
@COSTS@
};
const size_t archon_target_cost_count = @COST_COUNT@u;

const ArchonTargetScheduleFact archon_target_schedules[] = {
@SCHEDULES@
};
const size_t archon_target_schedule_count = @SCHEDULE_COUNT@u;

static int archon_target_range_valid(size_t first, size_t count, size_t total) {
    return first <= total && count <= total - first;
}

static int archon_target_knowledge_valid(ArchonTargetKnowledge knowledge) {
    return knowledge == ARCHON_TARGET_UNKNOWN || knowledge == ARCHON_TARGET_KNOWN;
}

const ArchonTargetArchitectureFact *archon_target_architecture(size_t index) {
    return index < archon_target_architecture_count
        ? &archon_target_architectures[index] : NULL;
}

const ArchonTargetArchitectureFact *archon_target_find_architecture(
        const char *identifier) {
    size_t i;
    if (identifier == NULL)
        return NULL;
    for (i = 0u; i < archon_target_architecture_count; ++i)
        if (strcmp(archon_target_architectures[i].identifier, identifier) == 0)
            return &archon_target_architectures[i];
    return NULL;
}

const ArchonTargetInstructionFact *archon_target_instruction(size_t index) {
    return index < archon_target_instruction_count
        ? &archon_target_instructions[index] : NULL;
}

const ArchonTargetInstructionFact *archon_target_find_instruction(
        const ArchonTargetArchitectureFact *target, const char *name) {
    size_t i;
    if (target == NULL || name == NULL ||
        !archon_target_range_valid(
            target->first_instruction, target->instruction_count,
            archon_target_instruction_count))
        return NULL;
    for (i = 0u; i < target->instruction_count; ++i) {
        const ArchonTargetInstructionFact *instruction =
            &archon_target_instructions[target->first_instruction + i];
        if (strcmp(instruction->name, name) == 0)
            return instruction;
    }
    return NULL;
}

const ArchonTargetAliasFact *archon_target_find_alias(
        const ArchonTargetArchitectureFact *target, const char *name) {
    size_t i;
    if (target == NULL || name == NULL ||
        !archon_target_range_valid(
            target->first_alias, target->alias_count, archon_target_alias_count))
        return NULL;
    for (i = 0u; i < target->alias_count; ++i) {
        const ArchonTargetAliasFact *alias =
            &archon_target_aliases[target->first_alias + i];
        if (strcmp(alias->name, name) == 0)
            return alias;
    }
    return NULL;
}

static int archon_target_validate_architectures(void) {
    size_t i, j;
    for (i = 0u; i < archon_target_architecture_count; ++i) {
        const ArchonTargetArchitectureFact *target = &archon_target_architectures[i];
        if (target->index != i || target->identifier == NULL ||
            target->identifier[0] == '\0' || target->name == NULL ||
            target->fingerprint == NULL || strlen(target->fingerprint) != 64u ||
            target->configuration_json == NULL || target->address_width == 0u ||
            (target->endian != ARCHON_TARGET_ENDIAN_LITTLE &&
             target->endian != ARCHON_TARGET_ENDIAN_BIG) ||
            !archon_target_knowledge_valid(target->abi_knowledge) ||
            (target->abi_knowledge == ARCHON_TARGET_KNOWN) !=
                (target->abi_json != NULL) ||
            !archon_target_range_valid(
                target->first_register, target->register_fact_count,
                archon_target_register_count) ||
            !archon_target_range_valid(
                target->first_alias, target->alias_count,
                archon_target_alias_count) ||
            !archon_target_range_valid(
                target->first_state_resource, target->state_resource_count,
                archon_target_state_resource_count) ||
            !archon_target_range_valid(
                target->first_operand_class, target->operand_class_count,
                archon_target_operand_class_count) ||
            !archon_target_range_valid(
                target->first_instruction, target->instruction_count,
                archon_target_instruction_count))
            return 0;
        if (target->register_file == ARCHON_TARGET_PRESENT) {
            if (target->register_file_name == NULL ||
                target->register_prefix == NULL || target->register_count == 0u ||
                target->register_width == 0u ||
                target->register_fact_count != (size_t)target->register_count)
                return 0;
            if (target->zero_register == ARCHON_TARGET_PRESENT) {
                if (target->zero_register_index >= target->register_count)
                    return 0;
            } else if (target->zero_register != ARCHON_TARGET_ABSENT ||
                       target->zero_register_index != UINT32_MAX) {
                return 0;
            }
        } else if (target->register_file != ARCHON_TARGET_ABSENT ||
                   target->register_count != 0u || target->register_width != 0u ||
                   target->register_fact_count != 0u || target->alias_count != 0u ||
                   target->zero_register != ARCHON_TARGET_ABSENT ||
                   target->zero_register_index != UINT32_MAX) {
            return 0;
        }
        for (j = i + 1u; j < archon_target_architecture_count; ++j)
            if (strcmp(target->identifier,
                       archon_target_architectures[j].identifier) == 0)
                return 0;
    }
    return 1;
}

static int archon_target_validate_registers(void) {
    size_t i;
    for (i = 0u; i < archon_target_register_count; ++i) {
        const ArchonTargetRegisterFact *reg = &archon_target_registers[i];
        const ArchonTargetArchitectureFact *target =
            archon_target_architecture(reg->target_index);
        if (target == NULL || reg->name == NULL ||
            reg->index >= target->register_count ||
            (reg->is_zero != 0 &&
             (target->zero_register != ARCHON_TARGET_PRESENT ||
              reg->index != target->zero_register_index)))
            return 0;
    }
    for (i = 0u; i < archon_target_alias_count; ++i) {
        const ArchonTargetAliasFact *alias = &archon_target_aliases[i];
        const ArchonTargetArchitectureFact *target =
            archon_target_architecture(alias->target_index);
        if (target == NULL || alias->name == NULL ||
            alias->register_index >= target->register_count)
            return 0;
    }
    return 1;
}

static int archon_target_validate_resources_and_classes(void) {
    size_t i;
    for (i = 0u; i < archon_target_state_resource_count; ++i) {
        const ArchonTargetStateResourceFact *resource =
            &archon_target_state_resources[i];
        if (archon_target_architecture(resource->target_index) == NULL ||
            resource->name == NULL || resource->extent == 0u)
            return 0;
        if (resource->kind == ARCHON_TARGET_RESOURCE_MEMORY) {
            if (resource->value_kind != ARCHON_TARGET_VALUE_NONE ||
                resource->width != 0u)
                return 0;
        } else if ((resource->kind != ARCHON_TARGET_RESOURCE_SCALAR &&
                    resource->kind != ARCHON_TARGET_RESOURCE_STACK) ||
                   resource->value_kind < ARCHON_TARGET_VALUE_BITS ||
                   resource->value_kind > ARCHON_TARGET_VALUE_SINT ||
                   resource->width == 0u ||
                   (resource->kind == ARCHON_TARGET_RESOURCE_SCALAR &&
                    resource->extent != 1u)) {
            return 0;
        }
    }
    for (i = 0u; i < archon_target_operand_class_count; ++i) {
        const ArchonTargetOperandClassFact *operand_class =
            &archon_target_operand_classes[i];
        if (archon_target_architecture(operand_class->target_index) == NULL ||
            (operand_class->kind != ARCHON_TARGET_OPERAND_REGISTER &&
             operand_class->kind != ARCHON_TARGET_OPERAND_IMMEDIATE) ||
            operand_class->direction < ARCHON_TARGET_DIRECTION_IN ||
            operand_class->direction > ARCHON_TARGET_DIRECTION_INOUT ||
            operand_class->value_kind < ARCHON_TARGET_VALUE_BITS ||
            operand_class->value_kind > ARCHON_TARGET_VALUE_SINT ||
            operand_class->value_width == 0u ||
            operand_class->encoding_width == 0u)
            return 0;
        if ((operand_class->kind == ARCHON_TARGET_OPERAND_REGISTER &&
             (operand_class->is_signed ||
              operand_class->value_kind != ARCHON_TARGET_VALUE_BITS)) ||
            (operand_class->kind == ARCHON_TARGET_OPERAND_IMMEDIATE &&
             operand_class->direction != ARCHON_TARGET_DIRECTION_IN))
            return 0;
    }
    return 1;
}

static int archon_target_validate_instructions(void) {
    size_t i, j;
    if (archon_target_schedule_count != archon_target_instruction_count)
        return 0;
    for (i = 0u; i < archon_target_instruction_count; ++i) {
        const ArchonTargetInstructionFact *instruction =
            &archon_target_instructions[i];
        const ArchonTargetArchitectureFact *target =
            archon_target_architecture(instruction->target_index);
        if (instruction->index != i || target == NULL ||
            instruction->stable_id == NULL || instruction->name == NULL ||
            instruction->assembly == NULL || instruction->encoding_width == 0u ||
            instruction->encoding_width % 8u != 0u ||
            instruction->size_bytes != instruction->encoding_width / 8u ||
            instruction->encoding_mask_hex == NULL ||
            instruction->encoding_value_hex == NULL ||
            !archon_target_knowledge_valid(instruction->semantics_knowledge) ||
            !archon_target_knowledge_valid(instruction->effects_knowledge) ||
            !archon_target_knowledge_valid(
                instruction->semantic_equivalence_knowledge) ||
            (instruction->semantics_knowledge == ARCHON_TARGET_KNOWN) !=
                (instruction->semantics_json != NULL) ||
            instruction->effects_knowledge != instruction->semantics_knowledge ||
            !archon_target_range_valid(
                instruction->first_operand, instruction->operand_count,
                archon_target_operand_count) ||
            !archon_target_range_valid(
                instruction->first_encoding_field,
                instruction->encoding_field_count,
                archon_target_encoding_field_count) ||
            !archon_target_range_valid(
                instruction->first_effect, instruction->effect_count,
                archon_target_effect_count) ||
            !archon_target_range_valid(
                instruction->first_cost, instruction->cost_count,
                archon_target_cost_count) ||
            instruction->cost_count != 5u ||
            instruction->schedule_index >= archon_target_schedule_count)
            return 0;
        if (instruction->effects_knowledge == ARCHON_TARGET_UNKNOWN &&
            instruction->effect_count != 0u)
            return 0;
        for (j = 0u; j < instruction->operand_count; ++j) {
            const ArchonTargetOperandFact *operand =
                &archon_target_operands[instruction->first_operand + j];
            const ArchonTargetOperandClassFact *operand_class;
            if (operand->instruction_index != i || operand->position != j ||
                operand->name == NULL ||
                operand->operand_class_index >= archon_target_operand_class_count)
                return 0;
            operand_class =
                &archon_target_operand_classes[operand->operand_class_index];
            if (operand_class->target_index != instruction->target_index)
                return 0;
        }
        for (j = 0u; j < instruction->encoding_field_count; ++j) {
            const ArchonTargetEncodingFieldFact *field =
                &archon_target_encoding_fields[
                    instruction->first_encoding_field + j];
            if (field->instruction_index != i ||
                field->operand_index < instruction->first_operand ||
                field->operand_index >=
                    instruction->first_operand + instruction->operand_count ||
                field->width == 0u || field->lsb > instruction->encoding_width ||
                field->width > instruction->encoding_width - field->lsb)
                return 0;
        }
        for (j = i + 1u; j < target->first_instruction + target->instruction_count;
             ++j)
            if (strcmp(instruction->stable_id,
                       archon_target_instructions[j].stable_id) == 0 ||
                strcmp(instruction->name,
                       archon_target_instructions[j].name) == 0)
                return 0;
    }
    return 1;
}

static int archon_target_validate_effects_costs_schedules(void) {
    size_t i;
    for (i = 0u; i < archon_target_effect_count; ++i) {
        const ArchonTargetEffectFact *effect = &archon_target_effects[i];
        const ArchonTargetInstructionFact *instruction =
            archon_target_instruction(effect->instruction_index);
        if (instruction == NULL || effect->resource == NULL ||
            effect->kind < ARCHON_TARGET_EFFECT_READ_REGISTER ||
            effect->kind > ARCHON_TARGET_EFFECT_WRITE_MEMORY)
            return 0;
        if (effect->kind <= ARCHON_TARGET_EFFECT_WRITE_REGISTER) {
            if (effect->operand_position >= instruction->operand_count ||
                effect->state_resource_index != ARCHON_TARGET_STATE_RESOURCE_NONE)
                return 0;
        } else {
            const ArchonTargetStateResourceFact *resource;
            if (effect->operand_position != ARCHON_TARGET_OPERAND_NONE ||
                effect->state_resource_index >= archon_target_state_resource_count)
                return 0;
            resource = &archon_target_state_resources[effect->state_resource_index];
            if (resource->target_index != instruction->target_index)
                return 0;
        }
    }
    for (i = 0u; i < archon_target_cost_count; ++i) {
        const ArchonTargetCostFact *cost = &archon_target_costs[i];
        if (archon_target_instruction(cost->instruction_index) == NULL ||
            cost->kind < ARCHON_TARGET_COST_CODE_SIZE_BYTES ||
            cost->kind > ARCHON_TARGET_COST_ENERGY_PICOJOULES ||
            !archon_target_knowledge_valid(cost->knowledge) ||
            cost->denominator == 0u)
            return 0;
        if (cost->kind == ARCHON_TARGET_COST_CODE_SIZE_BYTES) {
            if (cost->knowledge != ARCHON_TARGET_KNOWN ||
                cost->numerator !=
                    archon_target_instructions[cost->instruction_index].size_bytes ||
                cost->denominator != 1u)
                return 0;
        } else if (cost->knowledge != ARCHON_TARGET_UNKNOWN ||
                   cost->numerator != 0u || cost->denominator != 1u) {
            return 0;
        }
    }
    for (i = 0u; i < archon_target_schedule_count; ++i) {
        const ArchonTargetScheduleFact *schedule = &archon_target_schedules[i];
        if (schedule->instruction_index != i ||
            schedule->policy != ARCHON_TARGET_SCHEDULE_CONSERVATIVE_UNKNOWN ||
            schedule->latency_knowledge != ARCHON_TARGET_UNKNOWN ||
            schedule->throughput_knowledge != ARCHON_TARGET_UNKNOWN ||
            schedule->micro_ops_knowledge != ARCHON_TARGET_UNKNOWN ||
            schedule->execution_resources_knowledge != ARCHON_TARGET_UNKNOWN ||
            !schedule->conservative_barrier || schedule->may_reorder ||
            schedule->may_speculate)
            return 0;
    }
    return 1;
}

int archon_target_facts_validate(void) {
    return archon_target_validate_architectures() &&
           archon_target_validate_registers() &&
           archon_target_validate_resources_and_classes() &&
           archon_target_validate_instructions() &&
           archon_target_validate_effects_costs_schedules();
}
"""
