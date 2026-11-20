"""Canonical, versioned C99 X-macro interchange for the IR pipeline.

The six ``*.i`` files described here are data, not C headers.  They deliberately
have no conventional include guard: a consumer may define a subset of the
callbacks and include a stream repeatedly.  Each pass receives a begin record,
zero or more body records, and an end record.  Missing callbacks are temporarily
defined as no-ops and are removed after the pass.

This module is intentionally independent from the current IR backends.  Peers
can import its schemas, escaping helpers, stable-ID function, validators, and
emitters without making an existing backend adopt the ABI all at once.

ABI compatibility policy
------------------------

``ABI_VERSION`` covers the common envelope and C token representation.
Every artifact also has a schema version.  A generated stream checks both at
preprocessing time, and repeats both values in its begin callback so non-C
readers can validate them as well.  IDs are nonzero 64-bit values; zero is the
only null reference.  Counts, ordinals, widths, flags, cycles, and slots are
fixed-range integers.  Arbitrary text is emitted as a UTF-8 C string, while
arbitrary symbolic names use an injective hex-encoded C identifier.

Program operand children share one indexed namespace across SSA references,
literal bit patterns, symbols, and text; attributes have a separate indexed
namespace.  Unknown MIR dependency latency is represented explicitly and never
defaults to one.  Schedule regions retain their final initiation interval and
producer.  RegMap live intervals are half-open ``[begin, end)`` ranges over the
deterministic event-position space.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from enum import Enum
import hashlib
import json
import re

from ..diagnostics import ArchonError


ABI_VERSION_MAJOR = 1
ABI_VERSION_MINOR = 0
ABI_VERSION_PATCH = 0
ABI_VERSION = (
    (ABI_VERSION_MAJOR << 16) | (ABI_VERSION_MINOR << 8) | ABI_VERSION_PATCH
)
SCHEMA_VERSION = 1
# The module is an ABI library and fixture emitter, not an automatically active
# code-generation stage.  Registry implementations can honor this marker
# without importing a special-case module name.
PIPELINE_STAGE = False

UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1
MAX_BIT_WIDTH = 1 << 20
MAX_DEPENDENCY_LATENCY = UINT32_MAX
MAX_ITERATION_DISTANCE = UINT32_MAX


class MacroABIError(ArchonError):
    """A malformed macro stream or an invalid value supplied to an emitter."""


class ArtifactKind(str, Enum):
    PROGRAM = "program"
    MIR = "mir"
    INSTR_MAP = "instr-map"
    SCHEDULED = "scheduled"
    REG_MAP = "reg-map"
    REGALLOCATED = "regallocated"

    @property
    def filename(self) -> str:
        return {
            ArtifactKind.PROGRAM: "Program.i",
            ArtifactKind.MIR: "MIR.i",
            ArtifactKind.INSTR_MAP: "InstrMap.i",
            ArtifactKind.SCHEDULED: "Scheduled.i",
            ArtifactKind.REG_MAP: "RegMap.i",
            ArtifactKind.REGALLOCATED: "Regallocated.i",
        }[self]


class FieldKind(str, Enum):
    ABI_VERSION = "ABI version"
    SCHEMA_VERSION = "schema version"
    ID = "nonzero uint64 ID"
    OPTIONAL_ID = "uint64 ID or zero"
    UINT32 = "uint32"
    UINT64 = "uint64"
    INT64 = "int64"
    BOOL = "boolean"
    STRING = "UTF-8 string"
    BIT_PATTERN = "arbitrary-width hexadecimal bit pattern"
    IDENTIFIER = "symbolic identifier"
    ENUM = "enumerator"


@dataclass(frozen=True)
class FieldSpec:
    name: str
    kind: FieldKind
    minimum: int | None = None
    maximum: int | None = None
    domain: str = ""
    values: tuple[str, ...] = ()
    allow_empty: bool = True


@dataclass(frozen=True)
class MacroSpec:
    name: str
    fields: tuple[FieldSpec, ...]
    sort_fields: tuple[int, ...] = ()


@dataclass(frozen=True)
class ArtifactSpec:
    kind: ArtifactKind
    prefix: str
    begin: MacroSpec
    records: tuple[MacroSpec, ...]
    end: MacroSpec
    metadata_fields: tuple[str, ...]
    count_fields: tuple[tuple[str, str], ...]
    forward_references: tuple[str, ...] = ()

    @property
    def filename(self) -> str:
        return self.kind.filename

    @property
    def schema_macro(self) -> str:
        return f"ARCHON_{self.prefix}_SCHEMA_VERSION"

    @property
    def active_macro(self) -> str:
        return f"ARCHON_MACRO_ABI_ACTIVE_{self.prefix}"

    @property
    def macros(self) -> tuple[MacroSpec, ...]:
        return (self.begin, *self.records, self.end)

    def macro(self, name: str) -> MacroSpec | None:
        for macro in self.macros:
            if macro.name == name:
                return macro
        return None


@dataclass(frozen=True)
class Record:
    """One artifact body callback.

    ``macro`` may be a full callback name (``ARCHON_PROGRAM_BLOCK``) or a
    suffix (``BLOCK``) when passed to an artifact-specific emitter.
    """

    macro: str
    args: tuple[object, ...] = ()

    def __init__(self, macro: str, *args: object):
        object.__setattr__(self, "macro", macro)
        object.__setattr__(self, "args", tuple(args))


@dataclass(frozen=True)
class MacroStream:
    """A validated in-memory stream."""

    artifact: ArtifactKind
    stream_id: int
    records: tuple[Record, ...] = ()
    metadata: Mapping[str, object] = field(default_factory=dict)
    declared_counts: Mapping[str, int] | None = None

    @property
    def filename(self) -> str:
        return self.artifact.filename


def _f(
    name: str,
    kind: FieldKind,
    *,
    minimum: int | None = None,
    maximum: int | None = None,
    domain: str = "",
    values: Sequence[str] = (),
    allow_empty: bool = True,
) -> FieldSpec:
    return FieldSpec(
        name,
        kind,
        minimum,
        maximum,
        domain,
        tuple(values),
        allow_empty,
    )


def _m(name: str, fields: Sequence[FieldSpec], *sort_fields: int) -> MacroSpec:
    return MacroSpec(name, tuple(fields), tuple(sort_fields))


_TYPES = (
    "ARCHON_TYPE_VOID",
    "ARCHON_TYPE_BOOL",
    "ARCHON_TYPE_BITS",
    "ARCHON_TYPE_UINT",
    "ARCHON_TYPE_SINT",
    "ARCHON_TYPE_FLOAT",
    "ARCHON_TYPE_ADDRESS",
    "ARCHON_TYPE_VECTOR",
    "ARCHON_TYPE_TOKEN",
    "ARCHON_TYPE_OPAQUE",
)
_EDGES = (
    "ARCHON_EDGE_NORMAL",
    "ARCHON_EDGE_TRUE",
    "ARCHON_EDGE_FALSE",
    "ARCHON_EDGE_EXCEPTION",
    "ARCHON_EDGE_FALLTHROUGH",
)
_DEPENDENCIES = (
    "ARCHON_DEP_DATA",
    "ARCHON_DEP_EFFECT",
    "ARCHON_DEP_CONTROL",
    "ARCHON_DEP_ORDER",
)
_LATENCY_KINDS = (
    "ARCHON_LATENCY_KNOWN",
    "ARCHON_LATENCY_UNKNOWN",
)
_SYMBOL_KINDS = (
    "ARCHON_SYMBOL_GLOBAL",
    "ARCHON_SYMBOL_FUNCTION",
    "ARCHON_SYMBOL_BLOCK",
    "ARCHON_SYMBOL_EXTERNAL",
    "ARCHON_SYMBOL_TLS",
    "ARCHON_SYMBOL_CONSTANT_POOL",
    "ARCHON_SYMBOL_STACK",
    "ARCHON_SYMBOL_OPAQUE",
)
_TEXT_KINDS = (
    "ARCHON_TEXT_PLAIN",
    "ARCHON_TEXT_ASSEMBLY",
    "ARCHON_TEXT_CONSTRAINT",
    "ARCHON_TEXT_METADATA",
    "ARCHON_TEXT_OPAQUE",
)
_ATTRIBUTE_ENCODINGS = (
    "ARCHON_ATTRIBUTE_BITS",
    "ARCHON_ATTRIBUTE_SIGNED_INTEGER",
    "ARCHON_ATTRIBUTE_UNSIGNED_INTEGER",
    "ARCHON_ATTRIBUTE_BOOLEAN",
    "ARCHON_ATTRIBUTE_TEXT",
    "ARCHON_ATTRIBUTE_SYMBOL",
    "ARCHON_ATTRIBUTE_JSON",
    "ARCHON_ATTRIBUTE_OPAQUE",
)
_MAP_ROLES = (
    "ARCHON_MAP_PRIMARY",
    "ARCHON_MAP_LOWERING",
    "ARCHON_MAP_FOLDED",
    "ARCHON_MAP_AUXILIARY",
)
_SCHEDULE_PRODUCERS = (
    "ARCHON_SCHEDULE_SAT",
    "ARCHON_SCHEDULE_FALLBACK",
)
_WRITE_POLICIES = (
    "ARCHON_WRITE_PRESERVE",
    "ARCHON_WRITE_ZERO_EXTEND",
    "ARCHON_WRITE_SIGN_EXTEND",
    "ARCHON_WRITE_REPLACE",
    "ARCHON_WRITE_DISCARD",
)
_REG_MAP_EVENTS = (
    "ARCHON_REG_EVENT_USE",
    "ARCHON_REG_EVENT_DEF",
    "ARCHON_REG_EVENT_EARLY_CLOBBER",
)
_LOCATIONS = (
    "ARCHON_LOCATION_REGISTER",
    "ARCHON_LOCATION_STACK",
    "ARCHON_LOCATION_REMATERIALIZED",
    "ARCHON_LOCATION_UNASSIGNED",
)
_EVENTS = (
    "ARCHON_EVENT_USE",
    "ARCHON_EVENT_DEF",
    "ARCHON_EVENT_SPILL",
    "ARCHON_EVENT_RELOAD",
    "ARCHON_EVENT_COPY",
)


def _envelope(prefix: str, metadata: Sequence[FieldSpec], counts: Sequence[FieldSpec]):
    begin = _m(
        f"ARCHON_{prefix}_BEGIN",
        (
            _f("abi_version", FieldKind.ABI_VERSION),
            _f("schema_version", FieldKind.SCHEMA_VERSION),
            _f("stream_id", FieldKind.ID, domain="stream"),
            *metadata,
            *counts,
        ),
    )
    end = _m(
        f"ARCHON_{prefix}_END",
        (_f("stream_id", FieldKind.ID, domain="stream"),),
    )
    return begin, end


_program_begin, _program_end = _envelope(
    "PROGRAM",
    (),
    (
        _f("block_count", FieldKind.UINT32),
        _f("operation_count", FieldKind.UINT32),
    ),
)
_mir_begin, _mir_end = _envelope(
    "MIR",
    (_f("program_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),),
    (
        _f("block_count", FieldKind.UINT32),
        _f("vreg_count", FieldKind.UINT32),
        _f("instruction_count", FieldKind.UINT32),
    ),
)
_instr_map_begin, _instr_map_end = _envelope(
    "INSTR_MAP",
    (
        _f("program_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),
        _f("mir_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),
    ),
    (_f("entry_count", FieldKind.UINT32),),
)
_scheduled_begin, _scheduled_end = _envelope(
    "SCHEDULED",
    (
        _f("mir_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),
        _f("instr_map_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),
    ),
    (
        _f("region_count", FieldKind.UINT32),
        _f("instruction_count", FieldKind.UINT32),
    ),
)
_reg_map_begin, _reg_map_end = _envelope(
    "REG_MAP",
    (
        _f("target", FieldKind.STRING),
        _f("scheduled_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),
        _f("mir_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),
    ),
    (
        _f("register_count", FieldKind.UINT32),
        _f("alias_count", FieldKind.UINT32),
        _f("reserved_count", FieldKind.UINT32),
        _f("vreg_count", FieldKind.UINT32),
        _f("interval_count", FieldKind.UINT32),
        _f("event_count", FieldKind.UINT32),
        _f("fixed_count", FieldKind.UINT32),
    ),
)
_regallocated_begin, _regallocated_end = _envelope(
    "REGALLOCATED",
    (
        _f("scheduled_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),
        _f("reg_map_stream_id", FieldKind.OPTIONAL_ID, domain="stream"),
    ),
    (
        _f("assignment_count", FieldKind.UINT32),
        _f("event_count", FieldKind.UINT32),
    ),
)


ARTIFACT_SPECS: Mapping[ArtifactKind, ArtifactSpec] = {
    ArtifactKind.PROGRAM: ArtifactSpec(
        ArtifactKind.PROGRAM,
        "PROGRAM",
        _program_begin,
        (
            _m(
                "ARCHON_PROGRAM_BLOCK",
                (
                    _f("id", FieldKind.ID, domain="program.block"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("name", FieldKind.STRING),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_PROGRAM_OP",
                (
                    _f("id", FieldKind.ID, domain="program.op"),
                    _f("block_id", FieldKind.ID, domain="program.block"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("opcode", FieldKind.IDENTIFIER, allow_empty=False),
                    _f("result_id", FieldKind.OPTIONAL_ID, domain="program.value"),
                    _f("type", FieldKind.ENUM, values=_TYPES),
                    _f("width", FieldKind.UINT32, maximum=MAX_BIT_WIDTH),
                    _f("operand_count", FieldKind.UINT32),
                    _f("attribute_count", FieldKind.UINT32),
                    _f("successor_count", FieldKind.UINT32),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                2,
                0,
            ),
            _m(
                "ARCHON_PROGRAM_OPERAND",
                (
                    _f("operation_id", FieldKind.ID, domain="program.op"),
                    _f("index", FieldKind.UINT32),
                    _f("value_id", FieldKind.ID, domain="program.value"),
                    _f("flags", FieldKind.UINT32),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_PROGRAM_LITERAL",
                (
                    _f("operation_id", FieldKind.ID, domain="program.op"),
                    _f("index", FieldKind.UINT32),
                    _f("type", FieldKind.ENUM, values=_TYPES),
                    _f("width", FieldKind.UINT32, minimum=1, maximum=MAX_BIT_WIDTH),
                    _f("bits", FieldKind.BIT_PATTERN),
                    _f("flags", FieldKind.UINT32),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_PROGRAM_SYMBOL",
                (
                    _f("operation_id", FieldKind.ID, domain="program.op"),
                    _f("index", FieldKind.UINT32),
                    _f("kind", FieldKind.ENUM, values=_SYMBOL_KINDS),
                    _f("name", FieldKind.STRING, allow_empty=False),
                    _f("addend", FieldKind.INT64),
                    _f("flags", FieldKind.UINT32),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_PROGRAM_TEXT",
                (
                    _f("operation_id", FieldKind.ID, domain="program.op"),
                    _f("index", FieldKind.UINT32),
                    _f("kind", FieldKind.ENUM, values=_TEXT_KINDS),
                    _f("text", FieldKind.STRING),
                    _f("flags", FieldKind.UINT32),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_PROGRAM_ATTRIBUTE",
                (
                    _f("operation_id", FieldKind.ID, domain="program.op"),
                    _f("index", FieldKind.UINT32),
                    _f("name", FieldKind.IDENTIFIER, allow_empty=False),
                    _f("type", FieldKind.ENUM, values=_TYPES),
                    _f("width", FieldKind.UINT32, maximum=MAX_BIT_WIDTH),
                    _f("encoding", FieldKind.ENUM, values=_ATTRIBUTE_ENCODINGS),
                    _f("value", FieldKind.STRING),
                    _f("flags", FieldKind.UINT32),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_PROGRAM_SUCCESSOR",
                (
                    _f("operation_id", FieldKind.ID, domain="program.op"),
                    _f("index", FieldKind.UINT32),
                    _f("block_id", FieldKind.ID, domain="program.block"),
                    _f("kind", FieldKind.ENUM, values=_EDGES),
                ),
                0,
                1,
            ),
        ),
        _program_end,
        (),
        (
            ("block_count", "ARCHON_PROGRAM_BLOCK"),
            ("operation_count", "ARCHON_PROGRAM_OP"),
        ),
        (
            "operation-to-block",
            "operand-to-value (for phi/backedges)",
            "literal/symbol/text/attribute-to-operation",
            "successor-to-block",
        ),
    ),
    ArtifactKind.MIR: ArtifactSpec(
        ArtifactKind.MIR,
        "MIR",
        _mir_begin,
        (
            _m(
                "ARCHON_MIR_BLOCK",
                (
                    _f("id", FieldKind.ID, domain="mir.block"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("name", FieldKind.STRING),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_MIR_VREG",
                (
                    _f("id", FieldKind.ID, domain="mir.vreg"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("register_class", FieldKind.IDENTIFIER, allow_empty=False),
                    _f("width", FieldKind.UINT32, minimum=1, maximum=MAX_BIT_WIDTH),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_MIR_INSTRUCTION",
                (
                    _f("id", FieldKind.ID, domain="mir.instruction"),
                    _f("block_id", FieldKind.ID, domain="mir.block"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("opcode", FieldKind.IDENTIFIER, allow_empty=False),
                    _f("def_count", FieldKind.UINT32),
                    _f("use_count", FieldKind.UINT32),
                    _f("immediate_count", FieldKind.UINT32),
                    _f("successor_count", FieldKind.UINT32),
                    _f("dependency_count", FieldKind.UINT32),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                2,
                0,
            ),
            _m(
                "ARCHON_MIR_DEF",
                (
                    _f("instruction_id", FieldKind.ID, domain="mir.instruction"),
                    _f("index", FieldKind.UINT32),
                    _f("vreg_id", FieldKind.ID, domain="mir.vreg"),
                    _f("constraint", FieldKind.IDENTIFIER),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_MIR_USE",
                (
                    _f("instruction_id", FieldKind.ID, domain="mir.instruction"),
                    _f("index", FieldKind.UINT32),
                    _f("vreg_id", FieldKind.ID, domain="mir.vreg"),
                    _f("constraint", FieldKind.IDENTIFIER),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_MIR_IMMEDIATE",
                (
                    _f("instruction_id", FieldKind.ID, domain="mir.instruction"),
                    _f("index", FieldKind.UINT32),
                    _f("bits", FieldKind.UINT64),
                    _f("width", FieldKind.UINT32, minimum=1, maximum=64),
                    _f("is_signed", FieldKind.BOOL),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_MIR_SUCCESSOR",
                (
                    _f("instruction_id", FieldKind.ID, domain="mir.instruction"),
                    _f("index", FieldKind.UINT32),
                    _f("block_id", FieldKind.ID, domain="mir.block"),
                    _f("kind", FieldKind.ENUM, values=_EDGES),
                ),
                0,
                1,
            ),
            _m(
                "ARCHON_MIR_DEPENDENCY",
                (
                    _f("instruction_id", FieldKind.ID, domain="mir.instruction"),
                    _f("index", FieldKind.UINT32),
                    _f(
                        "predecessor_id",
                        FieldKind.ID,
                        domain="mir.instruction",
                    ),
                    _f("kind", FieldKind.ENUM, values=_DEPENDENCIES),
                    _f(
                        "latency_kind",
                        FieldKind.ENUM,
                        values=_LATENCY_KINDS,
                    ),
                    _f(
                        "latency",
                        FieldKind.UINT32,
                        maximum=MAX_DEPENDENCY_LATENCY,
                    ),
                    _f(
                        "iteration_distance",
                        FieldKind.UINT32,
                        maximum=MAX_ITERATION_DISTANCE,
                    ),
                ),
                0,
                1,
            ),
        ),
        _mir_end,
        ("program_stream_id",),
        (
            ("block_count", "ARCHON_MIR_BLOCK"),
            ("vreg_count", "ARCHON_MIR_VREG"),
            ("instruction_count", "ARCHON_MIR_INSTRUCTION"),
        ),
        (
            "instruction-to-block",
            "def/use-to-vreg",
            "successor-to-block",
            "positive-distance dependency-to-instruction recurrence",
        ),
    ),
    ArtifactKind.INSTR_MAP: ArtifactSpec(
        ArtifactKind.INSTR_MAP,
        "INSTR_MAP",
        _instr_map_begin,
        (
            _m(
                "ARCHON_INSTR_MAP_ENTRY",
                (
                    _f("id", FieldKind.ID, domain="instr-map.entry"),
                    _f("ordinal", FieldKind.UINT32),
                    _f(
                        "mir_instruction_id",
                        FieldKind.ID,
                        domain="mir.instruction",
                    ),
                    _f(
                        "target_instruction",
                        FieldKind.IDENTIFIER,
                        allow_empty=False,
                    ),
                    _f("emission_index", FieldKind.UINT32),
                    _f("role", FieldKind.ENUM, values=_MAP_ROLES),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
        ),
        _instr_map_end,
        ("program_stream_id", "mir_stream_id"),
        (("entry_count", "ARCHON_INSTR_MAP_ENTRY"),),
        ("entry-to-MIR-instruction (external stream)",),
    ),
    ArtifactKind.SCHEDULED: ArtifactSpec(
        ArtifactKind.SCHEDULED,
        "SCHEDULED",
        _scheduled_begin,
        (
            _m(
                "ARCHON_SCHEDULED_REGION",
                (
                    _f("id", FieldKind.ID, domain="scheduled.region"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("mir_block_id", FieldKind.ID, domain="mir.block"),
                    _f("initiation_interval", FieldKind.UINT32, minimum=1),
                    _f(
                        "producer",
                        FieldKind.ENUM,
                        values=_SCHEDULE_PRODUCERS,
                    ),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_SCHEDULED_INSTRUCTION",
                (
                    _f("id", FieldKind.ID, domain="scheduled.instruction"),
                    _f("ordinal", FieldKind.UINT32),
                    _f(
                        "instr_map_entry_id",
                        FieldKind.ID,
                        domain="instr-map.entry",
                    ),
                    _f("region_id", FieldKind.ID, domain="scheduled.region"),
                    _f("mir_block_id", FieldKind.ID, domain="mir.block"),
                    _f("cycle", FieldKind.UINT64),
                    _f("slot", FieldKind.UINT32),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
        ),
        _scheduled_end,
        ("mir_stream_id", "instr_map_stream_id"),
        (
            ("region_count", "ARCHON_SCHEDULED_REGION"),
            ("instruction_count", "ARCHON_SCHEDULED_INSTRUCTION"),
        ),
        (
            "instruction-to-instruction-map entry (external stream)",
            "instruction-to-schedule-region",
            "instruction-to-MIR block (external stream)",
        ),
    ),
    ArtifactKind.REG_MAP: ArtifactSpec(
        ArtifactKind.REG_MAP,
        "REG_MAP",
        _reg_map_begin,
        (
            _m(
                "ARCHON_REG_MAP_REGISTER",
                (
                    _f("id", FieldKind.ID, domain="register"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("name", FieldKind.STRING, allow_empty=False),
                    _f(
                        "register_class",
                        FieldKind.IDENTIFIER,
                        allow_empty=False,
                    ),
                    _f("width", FieldKind.UINT32, minimum=1, maximum=MAX_BIT_WIDTH),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_REG_MAP_ALIAS",
                (
                    _f("id", FieldKind.ID, domain="register.alias"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("name", FieldKind.STRING, allow_empty=False),
                    _f("register_id", FieldKind.ID, domain="register"),
                    _f("lsb", FieldKind.UINT32),
                    _f("width", FieldKind.UINT32, minimum=1, maximum=MAX_BIT_WIDTH),
                    _f("write_policy", FieldKind.ENUM, values=_WRITE_POLICIES),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_REG_MAP_RESERVED",
                (
                    _f("id", FieldKind.ID, domain="reg-map.reserved"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("register_id", FieldKind.ID, domain="register"),
                    _f("reason", FieldKind.STRING),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_REG_MAP_VREG",
                (
                    _f("vreg_id", FieldKind.ID, domain="mir.vreg"),
                    _f("ordinal", FieldKind.UINT32),
                    _f(
                        "register_class",
                        FieldKind.IDENTIFIER,
                        allow_empty=False,
                    ),
                    _f("width", FieldKind.UINT32, minimum=1, maximum=MAX_BIT_WIDTH),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_REG_MAP_INTERVAL",
                (
                    _f("id", FieldKind.ID, domain="reg-map.interval"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("vreg_id", FieldKind.ID, domain="mir.vreg"),
                    _f("begin", FieldKind.UINT64),
                    _f("end", FieldKind.UINT64),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_REG_MAP_EVENT",
                (
                    _f("id", FieldKind.ID, domain="reg-map.event"),
                    _f("ordinal", FieldKind.UINT32),
                    _f(
                        "scheduled_instruction_id",
                        FieldKind.ID,
                        domain="scheduled.instruction",
                    ),
                    _f("position", FieldKind.UINT64),
                    _f("kind", FieldKind.ENUM, values=_REG_MAP_EVENTS),
                    _f("vreg_id", FieldKind.ID, domain="mir.vreg"),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_REG_MAP_FIXED",
                (
                    _f("id", FieldKind.ID, domain="reg-map.fixed"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("vreg_id", FieldKind.ID, domain="mir.vreg"),
                    _f("register_id", FieldKind.ID, domain="register"),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
        ),
        _reg_map_end,
        ("target", "scheduled_stream_id", "mir_stream_id"),
        (
            ("register_count", "ARCHON_REG_MAP_REGISTER"),
            ("alias_count", "ARCHON_REG_MAP_ALIAS"),
            ("reserved_count", "ARCHON_REG_MAP_RESERVED"),
            ("vreg_count", "ARCHON_REG_MAP_VREG"),
            ("interval_count", "ARCHON_REG_MAP_INTERVAL"),
            ("event_count", "ARCHON_REG_MAP_EVENT"),
            ("fixed_count", "ARCHON_REG_MAP_FIXED"),
        ),
        (
            "alias-to-register",
            "reserved-to-register",
            "interval/event/fixed-to-vreg",
            "event-to-scheduled-instruction (external stream)",
            "fixed-vreg-to-register",
        ),
    ),
    ArtifactKind.REGALLOCATED: ArtifactSpec(
        ArtifactKind.REGALLOCATED,
        "REGALLOCATED",
        _regallocated_begin,
        (
            _m(
                "ARCHON_REGALLOCATED_ASSIGN",
                (
                    _f("id", FieldKind.ID, domain="regallocated.assignment"),
                    _f("ordinal", FieldKind.UINT32),
                    _f("vreg_id", FieldKind.ID, domain="mir.vreg"),
                    _f("location", FieldKind.ENUM, values=_LOCATIONS),
                    _f("register_id", FieldKind.OPTIONAL_ID, domain="register"),
                    _f("spill_slot", FieldKind.INT64, minimum=-1),
                    _f("width", FieldKind.UINT32, minimum=1, maximum=MAX_BIT_WIDTH),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
            _m(
                "ARCHON_REGALLOCATED_EVENT",
                (
                    _f("id", FieldKind.ID, domain="regallocated.event"),
                    _f("ordinal", FieldKind.UINT32),
                    _f(
                        "scheduled_instruction_id",
                        FieldKind.ID,
                        domain="scheduled.instruction",
                    ),
                    _f("kind", FieldKind.ENUM, values=_EVENTS),
                    _f("vreg_id", FieldKind.ID, domain="mir.vreg"),
                    _f("register_id", FieldKind.OPTIONAL_ID, domain="register"),
                    _f("spill_slot", FieldKind.INT64, minimum=-1),
                    _f("flags", FieldKind.UINT32),
                ),
                1,
                0,
            ),
        ),
        _regallocated_end,
        ("scheduled_stream_id", "reg_map_stream_id"),
        (
            ("assignment_count", "ARCHON_REGALLOCATED_ASSIGN"),
            ("event_count", "ARCHON_REGALLOCATED_EVENT"),
        ),
        (
            "assignment-to-vreg/register (external streams)",
            "event-to-scheduled-instruction (external stream)",
        ),
    ),
}


_ARTIFACT_ALIASES: dict[str, ArtifactKind] = {}
for _kind, _spec in ARTIFACT_SPECS.items():
    for _alias in (
        _kind.value,
        _kind.value.replace("-", "_"),
        _kind.name,
        _kind.filename,
        _kind.filename.removesuffix(".i"),
        _spec.prefix,
    ):
        _ARTIFACT_ALIASES[_alias.lower().replace("_", "-")] = _kind


_C_IDENTIFIER = re.compile(r"^[A-Za-z][A-Za-z0-9_]*$")


def artifact_kind(value: ArtifactKind | str) -> ArtifactKind:
    """Return the canonical artifact enum, accepting filenames and aliases."""

    if isinstance(value, ArtifactKind):
        return value
    if not isinstance(value, str) or not value.strip():
        raise MacroABIError("macro artifact must be a nonempty string")
    key = value.strip().lower().replace("_", "-")
    result = _ARTIFACT_ALIASES.get(key)
    if result is None:
        choices = ", ".join(kind.filename for kind in ArtifactKind)
        raise MacroABIError(f"unknown macro artifact {value!r}; expected one of {choices}")
    return result


def artifact_spec(value: ArtifactKind | str) -> ArtifactSpec:
    return ARTIFACT_SPECS[artifact_kind(value)]


def c_identifier(value: str, *, prefix: str = "ARCHON_ID") -> str:
    """Encode arbitrary Unicode injectively as a safe C preprocessing token.

    UTF-8 bytes are represented as uppercase hexadecimal.  Encoding every byte,
    rather than merely replacing punctuation, prevents collisions such as
    ``"a-b"`` versus ``"a_b"`` and makes hostile preprocessor text inert.
    """

    if not isinstance(value, str):
        raise MacroABIError(f"C identifier input must be a string, got {type(value).__name__}")
    if not _C_IDENTIFIER.fullmatch(prefix) or prefix.startswith("_"):
        raise MacroABIError(f"C identifier prefix {prefix!r} is not a safe public identifier")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise MacroABIError(f"C identifier contains an invalid Unicode surrogate: {exc}") from exc
    return f"{prefix}_{encoded.hex().upper()}"


escape_identifier = c_identifier


def decode_identifier(value: str, *, prefix: str = "ARCHON_ID") -> str:
    """Inverse of :func:`c_identifier` for canonical tokens."""

    if not isinstance(value, str):
        raise MacroABIError("encoded C identifier must be a string")
    pattern = re.compile(rf"^{re.escape(prefix)}_([0-9A-F]*)$")
    match = pattern.fullmatch(value)
    if match is None or len(match.group(1)) % 2:
        raise MacroABIError(f"noncanonical encoded identifier {value!r}")
    try:
        return bytes.fromhex(match.group(1)).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise MacroABIError(f"encoded identifier {value!r} is not UTF-8") from exc


def c_string(value: str) -> str:
    """Return an ASCII-only C99 string literal containing UTF-8 bytes.

    Question marks are octal-escaped to prevent C99 trigraph formation.  Octal
    escapes always contain exactly three digits, so adjacent characters cannot
    extend an escape.
    """

    if not isinstance(value, str):
        raise MacroABIError(f"C string input must be a string, got {type(value).__name__}")
    try:
        data = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise MacroABIError(f"C string contains an invalid Unicode surrogate: {exc}") from exc
    result = ['"']
    named = {
        0x07: r"\a",
        0x08: r"\b",
        0x09: r"\t",
        0x0A: r"\n",
        0x0B: r"\v",
        0x0C: r"\f",
        0x0D: r"\r",
        0x22: r"\"",
        0x5C: r"\\",
    }
    for byte in data:
        if byte in named:
            result.append(named[byte])
        elif 0x20 <= byte <= 0x7E and byte != 0x3F:
            result.append(chr(byte))
        else:
            result.append(f"\\{byte:03o}")
    result.append('"')
    return "".join(result)


escape_c_string = c_string


def unescape_c_string(literal: str) -> str:
    """Decode the single C literal form emitted by :func:`c_string`."""

    if not isinstance(literal, str) or len(literal) < 2:
        raise MacroABIError("C string literal is missing quotes")
    if literal[0] != '"' or literal[-1] != '"':
        raise MacroABIError(f"expected one C string literal, got {literal!r}")
    data = bytearray()
    index = 1
    named = {
        "a": 0x07,
        "b": 0x08,
        "t": 0x09,
        "n": 0x0A,
        "v": 0x0B,
        "f": 0x0C,
        "r": 0x0D,
        '"': 0x22,
        "\\": 0x5C,
        "?": 0x3F,
        "'": 0x27,
    }
    while index < len(literal) - 1:
        char = literal[index]
        if char == '"':
            raise MacroABIError("unescaped quote in C string literal")
        if char != "\\":
            code = ord(char)
            if code < 0x20 or code > 0x7E:
                raise MacroABIError("C string literal must be ASCII-only")
            data.append(code)
            index += 1
            continue
        index += 1
        if index >= len(literal) - 1:
            raise MacroABIError("trailing backslash in C string literal")
        escape = literal[index]
        if escape in named:
            data.append(named[escape])
            index += 1
        elif escape in "01234567":
            end = index
            while end < min(index + 3, len(literal) - 1) and literal[end] in "01234567":
                end += 1
            data.append(int(literal[index:end], 8))
            index = end
        else:
            raise MacroABIError(f"unsupported C escape \\{escape}")
    try:
        return bytes(data).decode("utf-8")
    except UnicodeDecodeError as exc:
        raise MacroABIError("C string literal does not contain valid UTF-8") from exc


def stable_id(namespace: str, *parts: object) -> int:
    """Return a deterministic, nonzero 64-bit semantic ID.

    Inputs use canonical JSON plus explicit length framing.  Mapping key order
    therefore has no effect, and tuples such as ``("ab", "c")`` cannot collide
    structurally with ``("a", "bc")`` before the cryptographic hash.
    """

    if not isinstance(namespace, str) or not namespace:
        raise MacroABIError("stable-ID namespace must be a nonempty string")
    digest = hashlib.sha256()
    digest.update(b"Archon Macro ABI stable ID v1\0")
    values = (namespace, *parts)
    for index, value in enumerate(values):
        try:
            payload = json.dumps(
                value,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
                allow_nan=False,
            ).encode("utf-8")
        except (TypeError, ValueError, UnicodeError, RecursionError) as exc:
            raise MacroABIError(
                f"stable-ID component {index} is not canonical JSON: {exc}"
            ) from exc
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    result = int.from_bytes(digest.digest()[:8], "big")
    return result or 1


def record(macro: str, *args: object, **fields: object) -> Record | Mapping[str, object]:
    """Convenience constructor for positional or named records."""

    if args and fields:
        raise MacroABIError("record() accepts positional arguments or named fields, not both")
    if fields:
        return {"macro": macro, **fields}
    return Record(macro, *args)


def _diagnostic(
    spec: ArtifactSpec,
    message: str,
    *,
    record_index: int | None = None,
    macro: str | None = None,
    field_name: str | None = None,
    line: int | None = None,
) -> MacroABIError:
    context = spec.filename
    if line is not None:
        context += f":{line}"
    elif record_index is not None:
        context += f": record {record_index + 1}"
    if macro:
        context += f" {macro}"
    if field_name:
        context += f" argument {field_name!r}"
    return MacroABIError(f"{context}: {message}")


def _integer(
    value: object,
    field_spec: FieldSpec,
    spec: ArtifactSpec,
    record_index: int,
    macro: str,
) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise _diagnostic(
            spec,
            f"expected {field_spec.kind.value}, got {type(value).__name__}",
            record_index=record_index,
            macro=macro,
            field_name=field_spec.name,
        )
    bounds = {
        FieldKind.ID: (1, UINT64_MAX),
        FieldKind.OPTIONAL_ID: (0, UINT64_MAX),
        FieldKind.UINT32: (0, UINT32_MAX),
        FieldKind.UINT64: (0, UINT64_MAX),
        FieldKind.INT64: (INT64_MIN, INT64_MAX),
        FieldKind.BOOL: (0, 1),
    }
    low, high = bounds[field_spec.kind]
    if field_spec.minimum is not None:
        low = max(low, field_spec.minimum)
    if field_spec.maximum is not None:
        high = min(high, field_spec.maximum)
    if not low <= value <= high:
        raise _diagnostic(
            spec,
            f"value {value} is outside [{low}, {high}]",
            record_index=record_index,
            macro=macro,
            field_name=field_spec.name,
        )
    return value


def _normalise_value(
    value: object,
    field_spec: FieldSpec,
    spec: ArtifactSpec,
    record_index: int,
    macro: str,
) -> object:
    kind = field_spec.kind
    if kind == FieldKind.ABI_VERSION:
        if value != ABI_VERSION:
            raise _diagnostic(
                spec,
                f"incompatible ABI version {value!r}; expected {ABI_VERSION}",
                record_index=record_index,
                macro=macro,
                field_name=field_spec.name,
            )
        return ABI_VERSION
    if kind == FieldKind.SCHEMA_VERSION:
        if value != SCHEMA_VERSION:
            raise _diagnostic(
                spec,
                f"incompatible schema version {value!r}; expected {SCHEMA_VERSION}",
                record_index=record_index,
                macro=macro,
                field_name=field_spec.name,
            )
        return SCHEMA_VERSION
    if kind in (FieldKind.ID, FieldKind.OPTIONAL_ID):
        if isinstance(value, str):
            value = stable_id(field_spec.domain or "id", value)
        return _integer(value, field_spec, spec, record_index, macro)
    if kind == FieldKind.BOOL:
        if isinstance(value, bool):
            return int(value)
        return _integer(value, field_spec, spec, record_index, macro)
    if kind in (
        FieldKind.UINT32,
        FieldKind.UINT64,
        FieldKind.INT64,
    ):
        return _integer(value, field_spec, spec, record_index, macro)
    if kind == FieldKind.BIT_PATTERN:
        if isinstance(value, bool):
            value = -1
        if isinstance(value, int):
            if value >= 0:
                return f"{value:X}"
        elif isinstance(value, str):
            digits = value[2:] if value.startswith(("0x", "0X")) else value
            if digits and re.fullmatch(r"[0-9A-Fa-f]+", digits):
                return f"{int(digits, 16):X}"
        raise _diagnostic(
            spec,
            f"expected a nonnegative integer or hexadecimal string, got {value!r}",
            record_index=record_index,
            macro=macro,
            field_name=field_spec.name,
        )
    if kind in (FieldKind.STRING, FieldKind.IDENTIFIER):
        if not isinstance(value, str):
            raise _diagnostic(
                spec,
                f"expected {kind.value}, got {type(value).__name__}",
                record_index=record_index,
                macro=macro,
                field_name=field_spec.name,
            )
        try:
            value.encode("utf-8")
        except UnicodeEncodeError as exc:
            raise _diagnostic(
                spec,
                f"invalid Unicode surrogate: {exc}",
                record_index=record_index,
                macro=macro,
                field_name=field_spec.name,
            ) from exc
        if not field_spec.allow_empty and not value:
            raise _diagnostic(
                spec,
                "must not be empty",
                record_index=record_index,
                macro=macro,
                field_name=field_spec.name,
            )
        if kind == FieldKind.STRING and "\0" in value:
            raise _diagnostic(
                spec,
                "embedded NUL is not representable as a C ABI string",
                record_index=record_index,
                macro=macro,
                field_name=field_spec.name,
            )
        return value
    if kind == FieldKind.ENUM:
        if not isinstance(value, str) or value not in field_spec.values:
            choices = ", ".join(field_spec.values)
            raise _diagnostic(
                spec,
                f"expected one of {choices}, got {value!r}",
                record_index=record_index,
                macro=macro,
                field_name=field_spec.name,
            )
        return value
    raise AssertionError(f"unhandled macro field kind {kind}")


def _resolve_macro_name(raw: object, spec: ArtifactSpec) -> str:
    if not isinstance(raw, str) or not raw:
        raise _diagnostic(spec, "record macro name must be a nonempty string")
    if spec.macro(raw) is not None:
        return raw
    candidate = f"ARCHON_{spec.prefix}_{raw.upper()}"
    if spec.macro(candidate) is not None:
        return candidate
    allowed = ", ".join(macro.name for macro in spec.records)
    raise _diagnostic(spec, f"unknown body macro {raw!r}; expected one of {allowed}")


def _coerce_record(
    raw: object,
    spec: ArtifactSpec,
    record_index: int,
) -> Record:
    if isinstance(raw, Record):
        macro_name = _resolve_macro_name(raw.macro, spec)
        values = raw.args
    elif isinstance(raw, Mapping):
        macro_name = _resolve_macro_name(raw.get("macro"), spec)
        macro_spec = spec.macro(macro_name)
        assert macro_spec is not None
        if "args" in raw:
            extras = set(raw) - {"macro", "args"}
            if extras:
                raise _diagnostic(
                    spec,
                    f"unexpected named fields with 'args': {', '.join(sorted(map(str, extras)))}",
                    record_index=record_index,
                    macro=macro_name,
                )
            args = raw["args"]
            if isinstance(args, (str, bytes)) or not isinstance(args, Sequence):
                raise _diagnostic(
                    spec,
                    "'args' must be a sequence",
                    record_index=record_index,
                    macro=macro_name,
                )
            values = tuple(args)
        else:
            expected = {field.name for field in macro_spec.fields}
            actual = set(raw) - {"macro"}
            missing = expected - actual
            extra = actual - expected
            if missing or extra:
                details = []
                if missing:
                    details.append(f"missing {', '.join(sorted(missing))}")
                if extra:
                    details.append(f"unexpected {', '.join(sorted(map(str, extra)))}")
                raise _diagnostic(
                    spec,
                    "; ".join(details),
                    record_index=record_index,
                    macro=macro_name,
                )
            values = tuple(raw[field.name] for field in macro_spec.fields)
    elif (
        isinstance(raw, Sequence)
        and not isinstance(raw, (str, bytes))
        and len(raw) >= 1
    ):
        macro_name = _resolve_macro_name(raw[0], spec)
        values = tuple(raw[1:])
    else:
        raise _diagnostic(
            spec,
            "record must be Record, mapping, or a sequence beginning with a macro name",
            record_index=record_index,
        )
    macro_spec = spec.macro(macro_name)
    assert macro_spec is not None
    if macro_spec is spec.begin or macro_spec is spec.end:
        raise _diagnostic(
            spec,
            "begin/end records are generated by the emitter and cannot appear in the body",
            record_index=record_index,
            macro=macro_name,
        )
    if len(values) != len(macro_spec.fields):
        raise _diagnostic(
            spec,
            f"wrong arity: got {len(values)}, expected {len(macro_spec.fields)}",
            record_index=record_index,
            macro=macro_name,
        )
    normalised = tuple(
        _normalise_value(value, field_spec, spec, record_index, macro_name)
        for value, field_spec in zip(values, macro_spec.fields)
    )
    return Record(macro_name, *normalised)


def _record_sort_key(record_value: Record, spec: ArtifactSpec):
    rank = {macro.name: index for index, macro in enumerate(spec.records)}
    macro_spec = spec.macro(record_value.macro)
    assert macro_spec is not None
    indexes = macro_spec.sort_fields or tuple(range(len(record_value.args)))
    return (
        rank[record_value.macro],
        *(record_value.args[index] for index in indexes),
        record_value.args,
    )


def _ensure_unique(
    spec: ArtifactSpec,
    rows: Sequence[Record],
    macro: str,
    indexes: tuple[int, ...],
    description: str,
) -> None:
    seen: dict[tuple[object, ...], int] = {}
    for record_index, row in enumerate(rows):
        if row.macro != macro:
            continue
        key = tuple(row.args[index] for index in indexes)
        previous = seen.get(key)
        if previous is not None:
            raise _diagnostic(
                spec,
                f"duplicate {description} {key!r}; first declared at record {previous + 1}",
                record_index=record_index,
                macro=macro,
            )
        seen[key] = record_index


def _ensure_contiguous(
    spec: ArtifactSpec,
    rows: Sequence[Record],
    macro: str,
    ordinal_index: int,
    *,
    group_index: int | None = None,
) -> None:
    groups: dict[object, list[int]] = {}
    for row in rows:
        if row.macro != macro:
            continue
        group = None if group_index is None else row.args[group_index]
        groups.setdefault(group, []).append(row.args[ordinal_index])
    for group, ordinals in groups.items():
        expected = list(range(len(ordinals)))
        if sorted(ordinals) != expected:
            suffix = "" if group is None else f" in group {group}"
            raise _diagnostic(
                spec,
                f"{macro} ordinals{suffix} must be contiguous from zero; "
                f"got {sorted(ordinals)!r}",
                macro=macro,
            )


def _indexed_children(
    spec: ArtifactSpec,
    rows: Sequence[Record],
    parent_macro: str,
    count_index: int,
    child_macro: str,
) -> None:
    parents = {row.args[0]: row for row in rows if row.macro == parent_macro}
    child_indexes: dict[int, list[int]] = {}
    for record_index, row in enumerate(rows):
        if row.macro != child_macro:
            continue
        parent = row.args[0]
        if parent not in parents:
            raise _diagnostic(
                spec,
                f"references unknown parent ID {parent}",
                record_index=record_index,
                macro=child_macro,
            )
        child_indexes.setdefault(parent, []).append(row.args[1])
    for parent, row in parents.items():
        expected_count = row.args[count_index]
        actual = sorted(child_indexes.get(parent, ()))
        expected = list(range(expected_count))
        if actual != expected:
            raise _diagnostic(
                spec,
                f"{child_macro} indices for parent {parent} must be {expected!r}; "
                f"got {actual!r}",
                macro=parent_macro,
            )


def _indexed_children_across(
    spec: ArtifactSpec,
    rows: Sequence[Record],
    parent_macro: str,
    count_index: int,
    child_macros: Sequence[str],
    description: str,
) -> None:
    """Validate one shared index space populated by several child kinds."""

    parents = {row.args[0]: row for row in rows if row.macro == parent_macro}
    children = set(child_macros)
    child_indexes: dict[int, list[tuple[int, str, int]]] = {}
    for record_index, row in enumerate(rows):
        if row.macro not in children:
            continue
        parent = row.args[0]
        if parent not in parents:
            raise _diagnostic(
                spec,
                f"references unknown parent ID {parent}",
                record_index=record_index,
                macro=row.macro,
            )
        child_indexes.setdefault(parent, []).append(
            (row.args[1], row.macro, record_index)
        )
    for parent, parent_row in parents.items():
        expected_count = parent_row.args[count_index]
        entries = child_indexes.get(parent, ())
        indexes = [entry[0] for entry in entries]
        expected = list(range(expected_count))
        if sorted(indexes) != expected:
            raise _diagnostic(
                spec,
                f"{description} indices for parent {parent} must be {expected!r}; "
                f"got {sorted(indexes)!r}",
                macro=parent_macro,
            )


def _validate_program(spec: ArtifactSpec, rows: Sequence[Record]) -> None:
    _ensure_unique(spec, rows, "ARCHON_PROGRAM_BLOCK", (0,), "block ID")
    _ensure_unique(spec, rows, "ARCHON_PROGRAM_BLOCK", (1,), "block ordinal")
    _ensure_unique(spec, rows, "ARCHON_PROGRAM_OP", (0,), "operation ID")
    _ensure_unique(spec, rows, "ARCHON_PROGRAM_OP", (1, 2), "operation position")
    _ensure_contiguous(spec, rows, "ARCHON_PROGRAM_BLOCK", 1)
    _ensure_contiguous(spec, rows, "ARCHON_PROGRAM_OP", 2, group_index=1)
    blocks = {row.args[0] for row in rows if row.macro == "ARCHON_PROGRAM_BLOCK"}
    values: set[int] = set()
    for record_index, row in enumerate(rows):
        if row.macro == "ARCHON_PROGRAM_OP":
            if row.args[1] not in blocks:
                raise _diagnostic(
                    spec,
                    f"references unknown block ID {row.args[1]}",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="block_id",
                )
            result, type_name, width = row.args[4:7]
            if result == 0:
                if type_name != "ARCHON_TYPE_VOID" or width != 0:
                    raise _diagnostic(
                        spec,
                        "an operation without a result must have void type and width zero",
                        record_index=record_index,
                        macro=row.macro,
                    )
            else:
                if result in values:
                    raise _diagnostic(
                        spec,
                        f"duplicate result value ID {result}",
                        record_index=record_index,
                        macro=row.macro,
                    )
                values.add(result)
                if type_name == "ARCHON_TYPE_VOID":
                    raise _diagnostic(
                        spec,
                        "an operation result cannot have void type",
                        record_index=record_index,
                        macro=row.macro,
                    )
                if type_name in ("ARCHON_TYPE_TOKEN",) and width != 0:
                    raise _diagnostic(
                        spec,
                        "token results must have width zero",
                        record_index=record_index,
                        macro=row.macro,
                    )
                if type_name not in ("ARCHON_TYPE_TOKEN", "ARCHON_TYPE_OPAQUE") and width == 0:
                    raise _diagnostic(
                        spec,
                        "value result width must be nonzero",
                        record_index=record_index,
                        macro=row.macro,
                    )
    for record_index, row in enumerate(rows):
        # Values were collected from the complete stream above, so this
        # intentionally permits phi/backedge forward references.
        if row.macro == "ARCHON_PROGRAM_OPERAND" and row.args[2] not in values:
            raise _diagnostic(
                spec,
                f"references unknown value ID {row.args[2]}",
                record_index=record_index,
                macro=row.macro,
                field_name="value_id",
            )
        elif row.macro == "ARCHON_PROGRAM_LITERAL":
            type_name, width, bits = row.args[2:5]
            if type_name in ("ARCHON_TYPE_VOID", "ARCHON_TYPE_TOKEN"):
                raise _diagnostic(
                    spec,
                    f"{type_name} cannot be used for a literal operand",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="type",
                )
            if type_name == "ARCHON_TYPE_BOOL" and width != 1:
                raise _diagnostic(
                    spec,
                    "boolean literal width must be one",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="width",
                )
            if int(bits, 16).bit_length() > width:
                raise _diagnostic(
                    spec,
                    f"literal bit pattern 0x{bits} does not fit width {width}",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="bits",
                )
        elif row.macro == "ARCHON_PROGRAM_ATTRIBUTE":
            type_name, width, encoding, value = row.args[3:7]
            if type_name == "ARCHON_TYPE_VOID":
                raise _diagnostic(
                    spec,
                    "attribute type cannot be void",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="type",
                )
            if (
                type_name not in ("ARCHON_TYPE_TOKEN", "ARCHON_TYPE_OPAQUE")
                and width == 0
            ):
                raise _diagnostic(
                    spec,
                    "typed attribute width must be nonzero",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="width",
                )
            if encoding == "ARCHON_ATTRIBUTE_BITS":
                if not re.fullmatch(r"(?:0|[1-9A-F][0-9A-F]*)", value):
                    raise _diagnostic(
                        spec,
                        "bits attribute value must be canonical uppercase hexadecimal",
                        record_index=record_index,
                        macro=row.macro,
                        field_name="value",
                    )
                if width == 0 or int(value, 16).bit_length() > width:
                    raise _diagnostic(
                        spec,
                        f"attribute bit pattern 0x{value} does not fit width {width}",
                        record_index=record_index,
                        macro=row.macro,
                        field_name="value",
                    )
        if row.macro == "ARCHON_PROGRAM_SUCCESSOR" and row.args[2] not in blocks:
            raise _diagnostic(
                spec,
                f"references unknown block ID {row.args[2]}",
                record_index=record_index,
                macro=row.macro,
                field_name="block_id",
            )
    _indexed_children_across(
        spec,
        rows,
        "ARCHON_PROGRAM_OP",
        7,
        (
            "ARCHON_PROGRAM_OPERAND",
            "ARCHON_PROGRAM_LITERAL",
            "ARCHON_PROGRAM_SYMBOL",
            "ARCHON_PROGRAM_TEXT",
        ),
        "operand child",
    )
    _indexed_children(
        spec,
        rows,
        "ARCHON_PROGRAM_OP",
        8,
        "ARCHON_PROGRAM_ATTRIBUTE",
    )
    _indexed_children(
        spec,
        rows,
        "ARCHON_PROGRAM_OP",
        9,
        "ARCHON_PROGRAM_SUCCESSOR",
    )


def _validate_mir(spec: ArtifactSpec, rows: Sequence[Record]) -> None:
    for macro, description in (
        ("ARCHON_MIR_BLOCK", "block"),
        ("ARCHON_MIR_VREG", "virtual register"),
        ("ARCHON_MIR_INSTRUCTION", "instruction"),
    ):
        _ensure_unique(spec, rows, macro, (0,), f"{description} ID")
        ordinal_key = (1,) if macro != "ARCHON_MIR_INSTRUCTION" else (1, 2)
        _ensure_unique(
            spec,
            rows,
            macro,
            ordinal_key,
            f"{description} ordinal/position",
        )
    _ensure_contiguous(spec, rows, "ARCHON_MIR_BLOCK", 1)
    _ensure_contiguous(spec, rows, "ARCHON_MIR_VREG", 1)
    _ensure_contiguous(spec, rows, "ARCHON_MIR_INSTRUCTION", 2, group_index=1)
    blocks = {row.args[0] for row in rows if row.macro == "ARCHON_MIR_BLOCK"}
    vregs = {row.args[0] for row in rows if row.macro == "ARCHON_MIR_VREG"}
    instructions = {
        row.args[0]: row for row in rows if row.macro == "ARCHON_MIR_INSTRUCTION"
    }
    for record_index, row in enumerate(rows):
        if row.macro == "ARCHON_MIR_INSTRUCTION" and row.args[1] not in blocks:
            raise _diagnostic(
                spec,
                f"references unknown block ID {row.args[1]}",
                record_index=record_index,
                macro=row.macro,
                field_name="block_id",
            )
        elif row.macro in ("ARCHON_MIR_DEF", "ARCHON_MIR_USE"):
            if row.args[2] not in vregs:
                raise _diagnostic(
                    spec,
                    f"references unknown virtual register ID {row.args[2]}",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="vreg_id",
                )
        elif row.macro == "ARCHON_MIR_IMMEDIATE":
            bits, width = row.args[2], row.args[3]
            if width < 64 and bits >= (1 << width):
                raise _diagnostic(
                    spec,
                    f"bit pattern 0x{bits:x} does not fit width {width}",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="bits",
                )
        elif row.macro == "ARCHON_MIR_SUCCESSOR" and row.args[2] not in blocks:
            raise _diagnostic(
                spec,
                f"references unknown block ID {row.args[2]}",
                record_index=record_index,
                macro=row.macro,
                field_name="block_id",
            )
        elif row.macro == "ARCHON_MIR_DEPENDENCY":
            parent = instructions.get(row.args[0])
            predecessor = instructions.get(row.args[2])
            latency_kind, latency, distance = row.args[4:7]
            if predecessor is None:
                raise _diagnostic(
                    spec,
                    f"references unknown predecessor instruction ID {row.args[2]}",
                    record_index=record_index,
                    macro=row.macro,
                )
            if latency_kind == "ARCHON_LATENCY_UNKNOWN" and latency != 0:
                raise _diagnostic(
                    spec,
                    "unknown dependency latency must carry zero, never an implicit default",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="latency",
                )
            if (
                distance == 0
                and parent is not None
                and (
                    parent.args[1] != predecessor.args[1]
                    or predecessor.args[2] >= parent.args[2]
                )
            ):
                raise _diagnostic(
                    spec,
                    "zero-distance dependency predecessor must be earlier "
                    "in the same block",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="iteration_distance",
                )
    for child, count_index in (
        ("ARCHON_MIR_DEF", 4),
        ("ARCHON_MIR_USE", 5),
        ("ARCHON_MIR_IMMEDIATE", 6),
        ("ARCHON_MIR_SUCCESSOR", 7),
        ("ARCHON_MIR_DEPENDENCY", 8),
    ):
        _indexed_children(
            spec,
            rows,
            "ARCHON_MIR_INSTRUCTION",
            count_index,
            child,
        )


def _validate_simple(spec: ArtifactSpec, rows: Sequence[Record]) -> None:
    macro = spec.records[0].name
    _ensure_unique(spec, rows, macro, (0,), "record ID")
    _ensure_unique(spec, rows, macro, (1,), "record ordinal")
    _ensure_contiguous(spec, rows, macro, 1)
    if spec.kind == ArtifactKind.INSTR_MAP:
        _ensure_unique(
            spec,
            rows,
            macro,
            (2, 4),
            "MIR instruction/emission index",
        )


def _validate_scheduled(spec: ArtifactSpec, rows: Sequence[Record]) -> None:
    region_macro = "ARCHON_SCHEDULED_REGION"
    instruction_macro = "ARCHON_SCHEDULED_INSTRUCTION"
    for macro, description in (
        (region_macro, "schedule region"),
        (instruction_macro, "scheduled instruction"),
    ):
        _ensure_unique(spec, rows, macro, (0,), f"{description} ID")
        _ensure_unique(spec, rows, macro, (1,), f"{description} ordinal")
        _ensure_contiguous(spec, rows, macro, 1)
    _ensure_unique(
        spec,
        rows,
        instruction_macro,
        (2,),
        "instruction-map entry",
    )
    _ensure_unique(
        spec,
        rows,
        instruction_macro,
        (3, 5, 6),
        "region/cycle/slot",
    )
    regions = {
        row.args[0]: row for row in rows if row.macro == region_macro
    }
    for record_index, row in enumerate(rows):
        if row.macro != instruction_macro:
            continue
        region = regions.get(row.args[3])
        if region is None:
            raise _diagnostic(
                spec,
                f"references unknown schedule region ID {row.args[3]}",
                record_index=record_index,
                macro=row.macro,
                field_name="region_id",
            )
        if row.args[4] != region.args[2]:
            raise _diagnostic(
                spec,
                f"instruction MIR block {row.args[4]} does not match "
                f"region MIR block {region.args[2]}",
                record_index=record_index,
                macro=row.macro,
                field_name="mir_block_id",
            )


def _validate_reg_map(spec: ArtifactSpec, rows: Sequence[Record]) -> None:
    for macro, description in (
        ("ARCHON_REG_MAP_REGISTER", "register"),
        ("ARCHON_REG_MAP_ALIAS", "alias"),
        ("ARCHON_REG_MAP_RESERVED", "reserved-register fact"),
        ("ARCHON_REG_MAP_VREG", "virtual register"),
        ("ARCHON_REG_MAP_INTERVAL", "live interval"),
        ("ARCHON_REG_MAP_EVENT", "use/def event"),
        ("ARCHON_REG_MAP_FIXED", "fixed constraint"),
    ):
        _ensure_unique(spec, rows, macro, (0,), f"{description} ID")
        _ensure_unique(spec, rows, macro, (1,), f"{description} ordinal")
        _ensure_contiguous(spec, rows, macro, 1)
    _ensure_unique(
        spec,
        rows,
        "ARCHON_REG_MAP_EVENT",
        (3, 4, 5),
        "position/kind/vreg event",
    )
    registers = {
        row.args[0]: row for row in rows if row.macro == "ARCHON_REG_MAP_REGISTER"
    }
    vregs = {
        row.args[0]: row for row in rows if row.macro == "ARCHON_REG_MAP_VREG"
    }
    names: set[str] = set()
    reserved_registers: set[int] = set()
    fixed_vregs: set[int] = set()
    intervals: dict[int, list[tuple[int, int, int]]] = {}
    for record_index, row in enumerate(rows):
        if row.macro == "ARCHON_REG_MAP_REGISTER":
            name = row.args[2]
            if name in names:
                raise _diagnostic(
                    spec,
                    f"duplicate register or alias name {name!r}",
                    record_index=record_index,
                    macro=row.macro,
                )
            names.add(name)
        elif row.macro == "ARCHON_REG_MAP_ALIAS":
            name, register_id, lsb, width = row.args[2:6]
            register = registers.get(register_id)
            if register is None:
                raise _diagnostic(
                    spec,
                    f"references unknown register ID {register_id}",
                    record_index=record_index,
                    macro=row.macro,
                )
            if name in names:
                raise _diagnostic(
                    spec,
                    f"duplicate register or alias name {name!r}",
                    record_index=record_index,
                    macro=row.macro,
                )
            names.add(name)
            if lsb + width > register.args[4]:
                raise _diagnostic(
                    spec,
                    f"alias range [{lsb}, {lsb + width}) exceeds register width "
                    f"{register.args[4]}",
                    record_index=record_index,
                    macro=row.macro,
                )
        elif row.macro == "ARCHON_REG_MAP_RESERVED":
            register_id = row.args[2]
            if register_id not in registers:
                raise _diagnostic(
                    spec,
                    f"references unknown register ID {register_id}",
                    record_index=record_index,
                    macro=row.macro,
                )
            if register_id in reserved_registers:
                raise _diagnostic(
                    spec,
                    f"duplicate reserved fact for register {register_id}",
                    record_index=record_index,
                    macro=row.macro,
                )
            reserved_registers.add(register_id)
        elif row.macro == "ARCHON_REG_MAP_INTERVAL":
            vreg, begin, end = row.args[2:5]
            if vreg not in vregs:
                raise _diagnostic(
                    spec,
                    f"references unknown virtual register ID {vreg}",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="vreg_id",
                )
            if begin >= end:
                raise _diagnostic(
                    spec,
                    f"live interval must be nonempty and half-open; "
                    f"got [{begin}, {end})",
                    record_index=record_index,
                    macro=row.macro,
                )
            intervals.setdefault(vreg, []).append((begin, end, record_index))
        elif row.macro == "ARCHON_REG_MAP_FIXED":
            vreg, register_id = row.args[2:4]
            vreg_record = vregs.get(vreg)
            if vreg_record is None:
                raise _diagnostic(
                    spec,
                    f"references unknown virtual register ID {vreg}",
                    record_index=record_index,
                    macro=row.macro,
                )
            register = registers.get(register_id)
            if register is None:
                raise _diagnostic(
                    spec,
                    f"references unknown register ID {register_id}",
                    record_index=record_index,
                    macro=row.macro,
                )
            if vreg_record.args[2] != register.args[3]:
                raise _diagnostic(
                    spec,
                    f"fixed register class {register.args[3]!r} does not match "
                    f"vreg class {vreg_record.args[2]!r}",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="register_id",
                )
            if register.args[4] < vreg_record.args[3]:
                raise _diagnostic(
                    spec,
                    f"fixed register width {register.args[4]} is narrower than "
                    f"vreg width {vreg_record.args[3]}",
                    record_index=record_index,
                    macro=row.macro,
                    field_name="register_id",
                )
            if vreg in fixed_vregs:
                raise _diagnostic(
                    spec,
                    f"duplicate fixed constraint for vreg {vreg}",
                    record_index=record_index,
                    macro=row.macro,
                )
            fixed_vregs.add(vreg)
    for vreg, ranges in intervals.items():
        ordered = sorted(ranges)
        for previous, current in zip(ordered, ordered[1:]):
            if previous[1] > current[0]:
                raise _diagnostic(
                    spec,
                    f"live intervals for vreg {vreg} overlap: "
                    f"[{previous[0]}, {previous[1]}) and "
                    f"[{current[0]}, {current[1]})",
                    record_index=current[2],
                    macro="ARCHON_REG_MAP_INTERVAL",
                )
    for record_index, row in enumerate(rows):
        if row.macro != "ARCHON_REG_MAP_EVENT":
            continue
        position, vreg = row.args[3], row.args[5]
        if vreg not in vregs:
            raise _diagnostic(
                spec,
                f"references unknown virtual register ID {vreg}",
                record_index=record_index,
                macro=row.macro,
                field_name="vreg_id",
            )
        if not any(
            begin <= position < end for begin, end, _ in intervals.get(vreg, ())
        ):
            raise _diagnostic(
                spec,
                f"event position {position} is outside every live interval "
                f"for vreg {vreg}",
                record_index=record_index,
                macro=row.macro,
                field_name="position",
            )


def _validate_regallocated(spec: ArtifactSpec, rows: Sequence[Record]) -> None:
    for macro, description in (
        ("ARCHON_REGALLOCATED_ASSIGN", "assignment"),
        ("ARCHON_REGALLOCATED_EVENT", "event"),
    ):
        _ensure_unique(spec, rows, macro, (0,), f"{description} ID")
        _ensure_unique(spec, rows, macro, (1,), f"{description} ordinal")
        _ensure_contiguous(spec, rows, macro, 1)
    assigned_vregs: set[int] = set()
    for record_index, row in enumerate(rows):
        if row.macro == "ARCHON_REGALLOCATED_ASSIGN":
            vreg, location, register_id, spill_slot = row.args[2:6]
            if vreg in assigned_vregs:
                raise _diagnostic(
                    spec,
                    f"duplicate assignment for vreg {vreg}",
                    record_index=record_index,
                    macro=row.macro,
                )
            assigned_vregs.add(vreg)
            if location == "ARCHON_LOCATION_REGISTER":
                valid = register_id != 0 and spill_slot == -1
            elif location == "ARCHON_LOCATION_STACK":
                valid = register_id == 0 and spill_slot >= 0
            else:
                valid = register_id == 0 and spill_slot == -1
            if not valid:
                raise _diagnostic(
                    spec,
                    "register/stack fields do not match the location kind",
                    record_index=record_index,
                    macro=row.macro,
                )
    for record_index, row in enumerate(rows):
        if row.macro != "ARCHON_REGALLOCATED_EVENT":
            continue
        kind, vreg, register_id, spill_slot = row.args[3:7]
        if vreg not in assigned_vregs:
            raise _diagnostic(
                spec,
                f"event references unassigned vreg {vreg}",
                record_index=record_index,
                macro=row.macro,
            )
        if kind in ("ARCHON_EVENT_SPILL", "ARCHON_EVENT_RELOAD") and spill_slot < 0:
            raise _diagnostic(
                spec,
                f"{kind} requires a nonnegative spill slot",
                record_index=record_index,
                macro=row.macro,
            )
        if kind == "ARCHON_EVENT_COPY" and register_id == 0:
            raise _diagnostic(
                spec,
                "copy event requires a physical register",
                record_index=record_index,
                macro=row.macro,
            )


def _validate_semantics(spec: ArtifactSpec, rows: Sequence[Record]) -> None:
    if spec.kind == ArtifactKind.PROGRAM:
        _validate_program(spec, rows)
    elif spec.kind == ArtifactKind.MIR:
        _validate_mir(spec, rows)
    elif spec.kind == ArtifactKind.INSTR_MAP:
        _validate_simple(spec, rows)
    elif spec.kind == ArtifactKind.SCHEDULED:
        _validate_scheduled(spec, rows)
    elif spec.kind == ArtifactKind.REG_MAP:
        _validate_reg_map(spec, rows)
    elif spec.kind == ArtifactKind.REGALLOCATED:
        _validate_regallocated(spec, rows)


def validate_records(
    artifact: ArtifactKind | str,
    records: Sequence[object],
) -> tuple[Record, ...]:
    """Validate and canonically sort artifact body records."""

    spec = artifact_spec(artifact)
    if isinstance(records, (str, bytes)) or not isinstance(records, Sequence):
        raise _diagnostic(spec, "records must be a sequence")
    normalised = tuple(
        _coerce_record(raw, spec, index) for index, raw in enumerate(records)
    )
    ordered = tuple(sorted(normalised, key=lambda row: _record_sort_key(row, spec)))
    _validate_semantics(spec, ordered)
    return ordered


def _metadata_defaults(kind: ArtifactKind) -> dict[str, object]:
    return {
        ArtifactKind.PROGRAM: {},
        ArtifactKind.MIR: {"program_stream_id": 0},
        ArtifactKind.INSTR_MAP: {
            "program_stream_id": 0,
            "mir_stream_id": 0,
        },
        ArtifactKind.SCHEDULED: {
            "mir_stream_id": 0,
            "instr_map_stream_id": 0,
        },
        ArtifactKind.REG_MAP: {
            "target": "",
            "scheduled_stream_id": 0,
            "mir_stream_id": 0,
        },
        ArtifactKind.REGALLOCATED: {
            "scheduled_stream_id": 0,
            "reg_map_stream_id": 0,
        },
    }[kind]


def _normalise_metadata(
    spec: ArtifactSpec,
    metadata: Mapping[str, object],
) -> dict[str, object]:
    defaults = _metadata_defaults(spec.kind)
    unknown = set(metadata) - set(spec.metadata_fields)
    if unknown:
        raise _diagnostic(
            spec,
            f"unknown stream metadata: {', '.join(sorted(map(str, unknown)))}",
        )
    values = {**defaults, **dict(metadata)}
    begin_by_name = {field.name: field for field in spec.begin.fields}
    result = {}
    for name in spec.metadata_fields:
        result[name] = _normalise_value(
            values[name],
            begin_by_name[name],
            spec,
            0,
            spec.begin.name,
        )
    return result


def _stream_fingerprint(
    spec: ArtifactSpec,
    records: Sequence[Record],
    metadata: Mapping[str, object],
) -> int:
    body = [
        {"macro": row.macro, "args": list(row.args)}
        for row in records
    ]
    return stable_id(
        "stream",
        spec.kind.value,
        dict(sorted(metadata.items())),
        body,
    )


def make_stream(
    artifact: ArtifactKind | str,
    records: Sequence[object] = (),
    *,
    stream_id: int | str | None = None,
    **metadata: object,
) -> MacroStream:
    """Build and validate a canonical stream without rendering it."""

    spec = artifact_spec(artifact)
    canonical_records = validate_records(spec.kind, records)
    canonical_metadata = _normalise_metadata(spec, metadata)
    if stream_id is None:
        actual_stream_id = _stream_fingerprint(
            spec,
            canonical_records,
            canonical_metadata,
        )
    else:
        actual_stream_id = _normalise_value(
            stream_id,
            _f("stream_id", FieldKind.ID, domain="stream"),
            spec,
            0,
            spec.begin.name,
        )
    return MacroStream(
        spec.kind,
        int(actual_stream_id),
        canonical_records,
        canonical_metadata,
    )


def _counts(spec: ArtifactSpec, rows: Sequence[Record]) -> dict[str, int]:
    return {
        field_name: sum(row.macro == macro_name for row in rows)
        for field_name, macro_name in spec.count_fields
    }


def _begin_values(stream: MacroStream, spec: ArtifactSpec) -> tuple[object, ...]:
    counts = _counts(spec, stream.records)
    return (
        ABI_VERSION,
        SCHEMA_VERSION,
        stream.stream_id,
        *(stream.metadata[name] for name in spec.metadata_fields),
        *(counts[name] for name, _ in spec.count_fields),
    )


def _format_value(value: object, field_spec: FieldSpec, spec: ArtifactSpec) -> str:
    kind = field_spec.kind
    if kind == FieldKind.ABI_VERSION:
        return "ARCHON_MACRO_ABI_VERSION"
    if kind == FieldKind.SCHEMA_VERSION:
        return spec.schema_macro
    if kind in (FieldKind.ID, FieldKind.OPTIONAL_ID, FieldKind.UINT64):
        return f"UINT64_C(0x{int(value):016X})"
    if kind == FieldKind.UINT32:
        return f"UINT32_C({int(value)})"
    if kind == FieldKind.INT64:
        number = int(value)
        return (
            f"(-INT64_C({-number}))"
            if number < 0
            else f"INT64_C({number})"
        )
    if kind == FieldKind.BOOL:
        return "1" if value else "0"
    if kind == FieldKind.STRING:
        return c_string(str(value))
    if kind == FieldKind.BIT_PATTERN:
        return c_string(str(value))
    if kind == FieldKind.IDENTIFIER:
        return c_identifier(str(value))
    if kind == FieldKind.ENUM:
        return str(value)
    raise AssertionError(f"cannot format {kind}")


def _format_invocation(
    macro_spec: MacroSpec,
    values: Sequence[object],
    spec: ArtifactSpec,
) -> str:
    return (
        f"{macro_spec.name}("
        + ", ".join(
            _format_value(value, field_spec, spec)
            for value, field_spec in zip(values, macro_spec.fields)
        )
        + ")"
    )


def _artifact_prelude(spec: ArtifactSpec) -> list[str]:
    lines = [
        f"/* Archon canonical {spec.filename}; generated, multi-pass X-macro data. */",
        '#include "MacroABI.h"',
        "",
        f"#if ARCHON_MACRO_ABI_VERSION != 0x{ABI_VERSION:08X}UL",
        f'# error "{spec.filename} requires Archon Macro ABI '
        f'{ABI_VERSION_MAJOR}.{ABI_VERSION_MINOR}.{ABI_VERSION_PATCH}"',
        "#endif",
        f"#if {spec.schema_macro} != {SCHEMA_VERSION}UL",
        f'# error "{spec.filename} schema version is incompatible with MacroABI.h"',
        "#endif",
        f"#ifdef {spec.active_macro}",
        f'# error "recursive inclusion of {spec.filename}"',
        "#endif",
        f"#define {spec.active_macro} 1",
        "",
    ]
    for macro_spec in spec.macros:
        local = f"ARCHON_MACRO_ABI_LOCAL_{macro_spec.name.removeprefix('ARCHON_')}"
        parameters = ", ".join(
            f"A{index}" for index in range(len(macro_spec.fields))
        )
        lines.extend(
            (
                f"#ifndef {macro_spec.name}",
                f"# define {macro_spec.name}({parameters})",
                f"# define {local} 1",
                "#endif",
            )
        )
    lines.append("")
    return lines


def _artifact_epilogue(spec: ArtifactSpec) -> list[str]:
    lines = [""]
    for macro_spec in reversed(spec.macros):
        local = f"ARCHON_MACRO_ABI_LOCAL_{macro_spec.name.removeprefix('ARCHON_')}"
        lines.extend(
            (
                f"#ifdef {local}",
                f"# undef {macro_spec.name}",
                f"# undef {local}",
                "#endif",
            )
        )
    lines.extend((f"#undef {spec.active_macro}", ""))
    return lines


def emit_stream(
    stream_or_artifact: MacroStream | ArtifactKind | str,
    records: Sequence[object] = (),
    *,
    stream_id: int | str | None = None,
    **metadata: object,
) -> str:
    """Render a canonical, deterministic, self-checking X-macro stream."""

    if isinstance(stream_or_artifact, MacroStream):
        if records or stream_id is not None or metadata:
            raise MacroABIError(
                "records, stream_id, and metadata cannot accompany a MacroStream"
            )
        stream = validate_stream(stream_or_artifact)
    else:
        stream = make_stream(
            stream_or_artifact,
            records,
            stream_id=stream_id,
            **metadata,
        )
    spec = artifact_spec(stream.artifact)
    lines = _artifact_prelude(spec)
    lines.append(_format_invocation(spec.begin, _begin_values(stream, spec), spec))
    for row in stream.records:
        macro_spec = spec.macro(row.macro)
        assert macro_spec is not None
        lines.append(_format_invocation(macro_spec, row.args, spec))
    lines.append(_format_invocation(spec.end, (stream.stream_id,), spec))
    lines.extend(_artifact_epilogue(spec))
    return "\n".join(lines)


def emit_empty_artifact(
    artifact: ArtifactKind | str,
    *,
    stream_id: int | str | None = None,
    **metadata: object,
) -> str:
    return emit_stream(artifact, (), stream_id=stream_id, **metadata)


def emit_program(
    records: Sequence[object] = (),
    *,
    stream_id: int | str | None = None,
) -> str:
    return emit_stream(ArtifactKind.PROGRAM, records, stream_id=stream_id)


def emit_mir(
    records: Sequence[object] = (),
    *,
    stream_id: int | str | None = None,
    program_stream_id: int | str = 0,
) -> str:
    return emit_stream(
        ArtifactKind.MIR,
        records,
        stream_id=stream_id,
        program_stream_id=program_stream_id,
    )


def emit_instr_map(
    records: Sequence[object] = (),
    *,
    stream_id: int | str | None = None,
    program_stream_id: int | str = 0,
    mir_stream_id: int | str = 0,
) -> str:
    return emit_stream(
        ArtifactKind.INSTR_MAP,
        records,
        stream_id=stream_id,
        program_stream_id=program_stream_id,
        mir_stream_id=mir_stream_id,
    )


def emit_scheduled(
    records: Sequence[object] = (),
    *,
    stream_id: int | str | None = None,
    mir_stream_id: int | str = 0,
    instr_map_stream_id: int | str = 0,
) -> str:
    return emit_stream(
        ArtifactKind.SCHEDULED,
        records,
        stream_id=stream_id,
        mir_stream_id=mir_stream_id,
        instr_map_stream_id=instr_map_stream_id,
    )


def emit_reg_map(
    records: Sequence[object] = (),
    *,
    stream_id: int | str | None = None,
    target: str = "",
    scheduled_stream_id: int | str = 0,
    mir_stream_id: int | str = 0,
) -> str:
    return emit_stream(
        ArtifactKind.REG_MAP,
        records,
        stream_id=stream_id,
        target=target,
        scheduled_stream_id=scheduled_stream_id,
        mir_stream_id=mir_stream_id,
    )


def emit_regallocated(
    records: Sequence[object] = (),
    *,
    stream_id: int | str | None = None,
    scheduled_stream_id: int | str = 0,
    reg_map_stream_id: int | str = 0,
) -> str:
    return emit_stream(
        ArtifactKind.REGALLOCATED,
        records,
        stream_id=stream_id,
        scheduled_stream_id=scheduled_stream_id,
        reg_map_stream_id=reg_map_stream_id,
    )


def _split_arguments(text: str) -> tuple[str, ...]:
    if not text.strip():
        return ()
    result: list[str] = []
    start = 0
    depth = 0
    quoted = False
    escaped = False
    for index, char in enumerate(text):
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            continue
        if char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            if depth == 0:
                raise MacroABIError("unbalanced ')' in macro arguments")
            depth -= 1
        elif char == "," and depth == 0:
            result.append(text[start:index].strip())
            start = index + 1
    if quoted:
        raise MacroABIError("unterminated C string in macro arguments")
    if depth:
        raise MacroABIError("unbalanced '(' in macro arguments")
    result.append(text[start:].strip())
    if any(not value for value in result):
        raise MacroABIError("empty macro argument")
    return tuple(result)


_UINT64_TOKEN = re.compile(r"UINT64_C\(0x([0-9A-Fa-f]{1,16})\)")
_UINT32_TOKEN = re.compile(r"UINT32_C\(([0-9]+)\)")
_INT64_TOKEN = re.compile(r"INT64_C\(([0-9]+)\)")
_NEG_INT64_TOKEN = re.compile(r"\(-INT64_C\(([0-9]+)\)\)")


def _parse_value(
    token: str,
    field_spec: FieldSpec,
    spec: ArtifactSpec,
    line: int,
    macro: str,
) -> object:
    kind = field_spec.kind
    try:
        if kind == FieldKind.ABI_VERSION:
            value: object = (
                ABI_VERSION
                if token == "ARCHON_MACRO_ABI_VERSION"
                else int(token.rstrip("uUlL"), 0)
            )
        elif kind == FieldKind.SCHEMA_VERSION:
            value = (
                SCHEMA_VERSION
                if token == spec.schema_macro
                else int(token.rstrip("uUlL"), 0)
            )
        elif kind in (FieldKind.ID, FieldKind.OPTIONAL_ID, FieldKind.UINT64):
            match = _UINT64_TOKEN.fullmatch(token)
            if match is None:
                raise ValueError("expected UINT64_C(0x...)")
            value = int(match.group(1), 16)
        elif kind == FieldKind.UINT32:
            match = _UINT32_TOKEN.fullmatch(token)
            if match is None:
                raise ValueError("expected UINT32_C(decimal)")
            value = int(match.group(1), 10)
        elif kind == FieldKind.INT64:
            negative = _NEG_INT64_TOKEN.fullmatch(token)
            positive = _INT64_TOKEN.fullmatch(token)
            if negative:
                value = -int(negative.group(1), 10)
            elif positive:
                value = int(positive.group(1), 10)
            else:
                raise ValueError("expected INT64_C(n) or (-INT64_C(n))")
        elif kind == FieldKind.BOOL:
            if token not in ("0", "1"):
                raise ValueError("expected 0 or 1")
            value = int(token)
        elif kind == FieldKind.STRING:
            value = unescape_c_string(token)
        elif kind == FieldKind.BIT_PATTERN:
            value = unescape_c_string(token)
        elif kind == FieldKind.IDENTIFIER:
            value = decode_identifier(token)
        elif kind == FieldKind.ENUM:
            value = token
        else:
            raise AssertionError(f"cannot parse {kind}")
        return _normalise_value(value, field_spec, spec, 0, macro)
    except MacroABIError as exc:
        raise _diagnostic(
            spec,
            exc.message,
            line=line,
            macro=macro,
            field_name=field_spec.name,
        ) from exc
    except (ValueError, OverflowError) as exc:
        raise _diagnostic(
            spec,
            str(exc),
            line=line,
            macro=macro,
            field_name=field_spec.name,
        ) from exc


_INVOCATION = re.compile(r"^(ARCHON_[A-Z0-9_]+)\((.*)\)$")


def parse_stream(
    text: str,
    artifact: ArtifactKind | str | None = None,
    *,
    require_envelope: bool = True,
    validate: bool = True,
) -> MacroStream:
    """Parse the canonical textual subset emitted by :func:`emit_stream`."""

    if not isinstance(text, str):
        raise MacroABIError(f"macro stream must be text, got {type(text).__name__}")
    hinted = artifact_spec(artifact) if artifact is not None else None
    if require_envelope and '#include "MacroABI.h"' not in text:
        if hinted is None:
            raise MacroABIError('macro stream is missing #include "MacroABI.h"')
        raise _diagnostic(hinted, 'missing #include "MacroABI.h"')
    invocations: list[tuple[int, str, tuple[str, ...]]] = []
    for line_number, source_line in enumerate(text.splitlines(), 1):
        stripped = source_line.strip()
        if not stripped or stripped.startswith(("#", "/*", "*", "*/", "//")):
            continue
        match = _INVOCATION.fullmatch(stripped)
        if match is None:
            if stripped.startswith("ARCHON_"):
                if hinted is None:
                    raise MacroABIError(
                        f"macro stream:{line_number}: malformed callback invocation"
                    )
                raise _diagnostic(
                    hinted,
                    "malformed callback invocation",
                    line=line_number,
                )
            continue
        try:
            arguments = _split_arguments(match.group(2))
        except MacroABIError as exc:
            if hinted is None:
                raise MacroABIError(f"macro stream:{line_number}: {exc}") from exc
            raise _diagnostic(hinted, str(exc), line=line_number) from exc
        invocations.append((line_number, match.group(1), arguments))
    if not invocations:
        if hinted is None:
            raise MacroABIError("macro stream contains no ABI callbacks")
        raise _diagnostic(hinted, "stream contains no ABI callbacks")
    if hinted is None:
        matches = [
            spec
            for spec in ARTIFACT_SPECS.values()
            if invocations[0][1] == spec.begin.name
        ]
        if len(matches) != 1:
            raise MacroABIError(
                f"macro stream:{invocations[0][0]}: first callback "
                f"{invocations[0][1]} is not a canonical begin callback"
            )
        spec = matches[0]
    else:
        spec = hinted
    if require_envelope:
        if f"#define {spec.active_macro} 1" not in text:
            raise _diagnostic(spec, f"missing active-inclusion sentinel {spec.active_macro}")
        if f"#undef {spec.active_macro}" not in text:
            raise _diagnostic(spec, f"active-inclusion sentinel {spec.active_macro} is not cleared")
    first_line, first_macro, first_tokens = invocations[0]
    if first_macro != spec.begin.name:
        raise _diagnostic(
            spec,
            f"first callback must be {spec.begin.name}, got {first_macro}",
            line=first_line,
        )
    if len(first_tokens) != len(spec.begin.fields):
        raise _diagnostic(
            spec,
            f"wrong begin arity: got {len(first_tokens)}, expected "
            f"{len(spec.begin.fields)}",
            line=first_line,
            macro=first_macro,
        )
    begin_values = tuple(
        _parse_value(token, field_spec, spec, first_line, first_macro)
        for token, field_spec in zip(first_tokens, spec.begin.fields)
    )
    stream_id = int(begin_values[2])
    metadata_start = 3
    metadata_end = metadata_start + len(spec.metadata_fields)
    metadata = dict(
        zip(spec.metadata_fields, begin_values[metadata_start:metadata_end])
    )
    count_names = tuple(name for name, _ in spec.count_fields)
    declared_counts = dict(zip(count_names, begin_values[metadata_end:]))
    body: list[Record] = []
    ended = False
    for line_number, macro_name, tokens in invocations[1:]:
        macro_spec = spec.macro(macro_name)
        if macro_spec is None:
            raise _diagnostic(
                spec,
                f"unknown callback {macro_name}",
                line=line_number,
            )
        if macro_spec is spec.begin:
            raise _diagnostic(spec, "duplicate begin callback", line=line_number)
        if ended:
            raise _diagnostic(
                spec,
                "callback appears after end callback",
                line=line_number,
                macro=macro_name,
            )
        if macro_spec is spec.end:
            if len(tokens) != 1:
                raise _diagnostic(
                    spec,
                    f"wrong end arity: got {len(tokens)}, expected 1",
                    line=line_number,
                    macro=macro_name,
                )
            end_id = _parse_value(
                tokens[0],
                macro_spec.fields[0],
                spec,
                line_number,
                macro_name,
            )
            if end_id != stream_id:
                raise _diagnostic(
                    spec,
                    f"end stream ID {end_id} does not match begin ID {stream_id}",
                    line=line_number,
                    macro=macro_name,
                )
            ended = True
            continue
        if len(tokens) != len(macro_spec.fields):
            raise _diagnostic(
                spec,
                f"wrong arity: got {len(tokens)}, expected {len(macro_spec.fields)}",
                line=line_number,
                macro=macro_name,
            )
        values = tuple(
            _parse_value(token, field_spec, spec, line_number, macro_name)
            for token, field_spec in zip(tokens, macro_spec.fields)
        )
        body.append(Record(macro_name, *values))
    if not ended:
        raise _diagnostic(spec, f"missing {spec.end.name}")
    stream = MacroStream(
        spec.kind,
        stream_id,
        tuple(body),
        metadata,
        declared_counts,
    )
    return validate_stream(stream) if validate else stream


def validate_stream(
    stream: MacroStream | str,
    artifact: ArtifactKind | str | None = None,
) -> MacroStream:
    """Validate an in-memory or textual stream and return its canonical model."""

    if isinstance(stream, str):
        return parse_stream(stream, artifact, validate=True)
    if not isinstance(stream, MacroStream):
        raise MacroABIError(
            f"expected MacroStream or text, got {type(stream).__name__}"
        )
    spec = artifact_spec(stream.artifact)
    if artifact is not None and artifact_kind(artifact) != spec.kind:
        raise _diagnostic(
            spec,
            f"artifact hint {artifact_kind(artifact).filename} does not match stream",
        )
    stream_id = _normalise_value(
        stream.stream_id,
        _f("stream_id", FieldKind.ID, domain="stream"),
        spec,
        0,
        spec.begin.name,
    )
    metadata = _normalise_metadata(spec, stream.metadata)
    records = validate_records(spec.kind, stream.records)
    if tuple(stream.records) != records:
        raise _diagnostic(spec, "body records are not in canonical deterministic order")
    actual_counts = _counts(spec, records)
    if stream.declared_counts is not None:
        if set(stream.declared_counts) != set(actual_counts):
            raise _diagnostic(spec, "begin callback has an invalid count field set")
        for name, actual in actual_counts.items():
            declared = stream.declared_counts[name]
            if declared != actual:
                raise _diagnostic(
                    spec,
                    f"begin declares {name}={declared}, but body contains {actual}",
                    macro=spec.begin.name,
                )
    return MacroStream(
        spec.kind,
        int(stream_id),
        records,
        metadata,
        dict(actual_counts) if stream.declared_counts is not None else None,
    )


def _enum_declarations() -> str:
    groups = (
        ("archon_macro_type", _TYPES),
        ("archon_macro_edge", _EDGES),
        ("archon_macro_dependency", _DEPENDENCIES),
        ("archon_macro_latency_kind", _LATENCY_KINDS),
        ("archon_macro_symbol_kind", _SYMBOL_KINDS),
        ("archon_macro_text_kind", _TEXT_KINDS),
        ("archon_macro_attribute_encoding", _ATTRIBUTE_ENCODINGS),
        ("archon_macro_map_role", _MAP_ROLES),
        ("archon_macro_schedule_producer", _SCHEDULE_PRODUCERS),
        ("archon_macro_write_policy", _WRITE_POLICIES),
        ("archon_macro_reg_map_event", _REG_MAP_EVENTS),
        ("archon_macro_location", _LOCATIONS),
        ("archon_macro_event", _EVENTS),
    )
    chunks = []
    value = 0
    for enum_name, names in groups:
        rows = []
        for name in names:
            rows.append(f"    {name} = {value},")
            value += 1
        chunks.append(f"enum {enum_name} {{\n" + "\n".join(rows) + "\n};")
    return "\n\n".join(chunks)


def _callback_documentation() -> str:
    lines = [
        "/*",
        " * Callback signatures. Each .i file may be included more than once.",
        " * Define any callbacks needed by a pass; missing callbacks are no-ops.",
        " *",
        " * Conventions:",
        " *   Program OPERAND/LITERAL/SYMBOL/TEXT records share OP.OPERAND_COUNT",
        " *   and one contiguous INDEX space; ATTRIBUTE uses ATTRIBUTE_COUNT.",
        " *   LITERAL.BITS is canonical uppercase hexadecimal text without 0x.",
        " *   MIR LATENCY_UNKNOWN requires LATENCY == 0. DISTANCE == 0 requires",
        " *   an earlier predecessor in the same block; positive distance denotes",
        " *   a loop-carried recurrence and may reference self or a later instruction.",
        " *   SCHEDULED_REGION stores final INITIATION_INTERVAL and SAT/FALLBACK.",
        " *   REG_MAP_INTERVAL is half-open [BEGIN, END); each EVENT.POSITION must",
        " *   be covered by an interval belonging to EVENT.VREG_ID.",
    ]
    for spec in ARTIFACT_SPECS.values():
        lines.append(f" *")
        lines.append(f" * {spec.filename} (schema {SCHEMA_VERSION}):")
        for macro in spec.macros:
            fields = ", ".join(field.name.upper() for field in macro.fields)
            lines.append(f" *   {macro.name}({fields})")
        if spec.forward_references:
            lines.append(
                " *   Forward references allowed: "
                + "; ".join(spec.forward_references)
                + "."
            )
    lines.append(" */")
    return "\n".join(lines)


def macro_abi_header() -> str:
    """Return the canonical strict-C99 ``MacroABI.h`` text."""

    schema_defines = "\n".join(
        line
        for spec in ARTIFACT_SPECS.values()
        for line in (
            f"#if defined({spec.schema_macro}) && "
            f"{spec.schema_macro} != {SCHEMA_VERSION}UL",
            f'# error "incompatible predefined {spec.schema_macro}"',
            "#endif",
            f"#ifndef {spec.schema_macro}",
            f"# define {spec.schema_macro} {SCHEMA_VERSION}UL",
            "#endif",
        )
    )
    return f"""/* Archon canonical C99 X-macro ABI. Generated; do not edit. */
#ifndef ARCHON_MACRO_ABI_H_INCLUDED
#define ARCHON_MACRO_ABI_H_INCLUDED

#include <stdint.h>

#define ARCHON_MACRO_ABI_VERSION_MAJOR {ABI_VERSION_MAJOR}UL
#define ARCHON_MACRO_ABI_VERSION_MINOR {ABI_VERSION_MINOR}UL
#define ARCHON_MACRO_ABI_VERSION_PATCH {ABI_VERSION_PATCH}UL

#if defined(ARCHON_MACRO_ABI_VERSION) && \\
    ARCHON_MACRO_ABI_VERSION != 0x{ABI_VERSION:08X}UL
# error "incompatible predefined ARCHON_MACRO_ABI_VERSION"
#endif
#ifndef ARCHON_MACRO_ABI_VERSION
# define ARCHON_MACRO_ABI_VERSION 0x{ABI_VERSION:08X}UL
#endif

#if defined(ARCHON_MACRO_ABI_REQUIRE_MAJOR) && \\
    ARCHON_MACRO_ABI_REQUIRE_MAJOR != ARCHON_MACRO_ABI_VERSION_MAJOR
# error "unsupported Archon Macro ABI major version"
#endif
#if defined(ARCHON_MACRO_ABI_REQUIRE_MINOR) && \\
    ARCHON_MACRO_ABI_REQUIRE_MINOR > ARCHON_MACRO_ABI_VERSION_MINOR
# error "Archon Macro ABI minor version is too old"
#endif

{schema_defines}

{_enum_declarations()}

{_callback_documentation()}

#endif /* ARCHON_MACRO_ABI_H_INCLUDED */
"""


def _context_key(context: object) -> object:
    if context is None:
        return {}
    if not isinstance(context, Mapping):
        raise MacroABIError(
            f"macro ABI context must be a mapping or None, got {type(context).__name__}"
        )
    # The orchestration boundary may add compatibility aliases such as ``db``
    # and ``selected_databases``.  They must not perturb artifact identities:
    # the four canonical context keys are the stable generation contract.
    canonical_names = ("ir", "architectures", "architecture_names", "options")
    if any(name in context for name in canonical_names):
        source = {name: context[name] for name in canonical_names if name in context}
    else:
        source = dict(context)
    try:
        encoded = json.dumps(
            source,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        )
        return json.loads(encoded)
    except (TypeError, ValueError, UnicodeError, RecursionError) as exc:
        raise MacroABIError(f"macro ABI context must be canonical JSON: {exc}") from exc


def emit(context: Mapping[str, object] | None = None) -> dict[str, str]:
    """Emit ``MacroABI.h`` and a linked, representative empty pipeline.

    The empty streams are useful compile fixtures and unambiguous placeholders.
    Their IDs are stable for a canonical generation context.  They do not claim
    that a backend has produced a real program.
    """

    key = _context_key(context)
    program_id = stable_id("stream", "empty-program", key)
    mir_id = stable_id("stream", "empty-mir", key)
    instr_map_id = stable_id("stream", "empty-instr-map", key)
    scheduled_id = stable_id("stream", "empty-scheduled", key)
    reg_map_id = stable_id("stream", "empty-reg-map", key)
    regallocated_id = stable_id("stream", "empty-regallocated", key)
    target = ""
    if isinstance(context, Mapping):
        names = context.get("architecture_names")
        if isinstance(names, Sequence) and not isinstance(names, (str, bytes)):
            target = ",".join(str(name) for name in names)
    outputs = {
        "MacroABI.h": macro_abi_header(),
        "Program.i": emit_program(stream_id=program_id),
        "MIR.i": emit_mir(
            stream_id=mir_id,
            program_stream_id=program_id,
        ),
        "InstrMap.i": emit_instr_map(
            stream_id=instr_map_id,
            program_stream_id=program_id,
            mir_stream_id=mir_id,
        ),
        "Scheduled.i": emit_scheduled(
            stream_id=scheduled_id,
            mir_stream_id=mir_id,
            instr_map_stream_id=instr_map_id,
        ),
        "RegMap.i": emit_reg_map(
            stream_id=reg_map_id,
            target=target,
            scheduled_stream_id=scheduled_id,
            mir_stream_id=mir_id,
        ),
        "Regallocated.i": emit_regallocated(
            stream_id=regallocated_id,
            scheduled_stream_id=scheduled_id,
            reg_map_stream_id=reg_map_id,
        ),
    }
    return dict(sorted(outputs.items()))


__all__ = [
    "ABI_VERSION",
    "ABI_VERSION_MAJOR",
    "ABI_VERSION_MINOR",
    "ABI_VERSION_PATCH",
    "ARTIFACT_SPECS",
    "ArtifactKind",
    "ArtifactSpec",
    "FieldKind",
    "FieldSpec",
    "INT64_MAX",
    "INT64_MIN",
    "MAX_BIT_WIDTH",
    "MAX_DEPENDENCY_LATENCY",
    "MAX_ITERATION_DISTANCE",
    "MacroABIError",
    "MacroSpec",
    "MacroStream",
    "PIPELINE_STAGE",
    "Record",
    "SCHEMA_VERSION",
    "UINT32_MAX",
    "UINT64_MAX",
    "artifact_kind",
    "artifact_spec",
    "c_identifier",
    "c_string",
    "decode_identifier",
    "emit",
    "emit_empty_artifact",
    "emit_instr_map",
    "emit_mir",
    "emit_program",
    "emit_reg_map",
    "emit_regallocated",
    "emit_scheduled",
    "emit_stream",
    "escape_c_string",
    "escape_identifier",
    "macro_abi_header",
    "make_stream",
    "parse_stream",
    "record",
    "stable_id",
    "unescape_c_string",
    "validate_records",
    "validate_stream",
]
