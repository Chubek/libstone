"""Generate the target-independent ANF-to-MachineIR scheduler.

The generated scheduler performs a stable dependency-aware list schedule.  It
does not pretend that instruction selection has already happened: every MIR
operation retains a target-independent opcode and an explicit candidate set
computed from the selected ArchDBs.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from typing import Any

from ..diagnostics import ArchonError


NAME = "scheduler"
ALIASES = ("schedule", "list-scheduler", "machine-ir")


@dataclass(frozen=True)
class _Target:
    name: str
    address_width: int
    db: Mapping[str, Any]


@dataclass(frozen=True)
class _InstructionFact:
    target: int
    target_name: str
    ordinal: int
    instruction: str
    encoding_bytes: int
    encoding_bytes_known: bool


@dataclass(frozen=True)
class _Candidate:
    primitive: str
    result_width: int
    input_kinds: tuple[str, ...]
    input_widths: tuple[int, ...]
    fact: _InstructionFact


_SEMANTIC_PRIMITIVES = {
    "+%": "ADD",
    "-%": "SUB",
    "*%": "MUL",
    "&": "AND",
    "|": "OR",
    "^": "XOR",
    "~": "NOT",
    "<<": "SHL",
    "zext": "ZEXT",
    "sext": "SEXT",
    "trunc": "TRUNC",
    "bitcast": "BITCAST",
}


def _oracle_enabled(context: Any) -> bool:
    if not isinstance(context, Mapping):
        return True
    options = context.get("options", {})
    if options is None:
        options = {}
    if not isinstance(options, Mapping):
        raise ArchonError("scheduler options must be an object")
    enabled = options.get("oracle", True)
    if type(enabled) is not bool:
        raise ArchonError("scheduler option 'oracle' must be boolean")
    return enabled


def _member(context: Any, names: Iterable[str]) -> Any:
    for name in names:
        if isinstance(context, Mapping) and name in context:
            return context[name]
        if hasattr(context, name):
            return getattr(context, name)
    return None


def _unwrap_database(value: Any) -> Mapping[str, Any] | None:
    if isinstance(value, Mapping):
        if "instructions" in value and "architecture" in value:
            return value
        for key in ("db", "database", "archdb"):
            nested = value.get(key)
            if isinstance(nested, Mapping):
                return nested
    for key in ("db", "database", "archdb"):
        nested = getattr(value, key, None)
        if isinstance(nested, Mapping):
            return nested
    return None


def _selected_targets(context: Any) -> list[_Target]:
    raw = _member(
        context,
        (
            "selected_archdbs",
            "archdbs",
            "databases",
            "selected_databases",
            "targets",
            "architectures",
        ),
    )
    direct = _unwrap_database(context)
    if raw is None:
        raw = [direct] if direct is not None else []
    elif isinstance(raw, Mapping) and _unwrap_database(raw) is None:
        raw = [
            {"name": name, "database": database}
            for name, database in raw.items()
        ]
    elif not isinstance(raw, (list, tuple, set)):
        raw = [raw]

    targets: list[_Target] = []
    for item in raw:
        db = _unwrap_database(item)
        if db is None:
            continue
        override = None
        if isinstance(item, Mapping):
            override = item.get("target_name") or item.get("name")
        else:
            override = getattr(item, "target_name", None) or getattr(
                item, "name", None
            )
        name = str(override or db.get("architecture") or "target")
        try:
            address_width = int(db.get("address_width", 0))
        except (TypeError, ValueError):
            address_width = 0
        targets.append(_Target(name, max(0, address_width), db))

    # A mapping or a set must not make output ordering depend on hash order.
    targets.sort(key=lambda target: (target.name, str(target.db.get("architecture", ""))))
    unique: list[_Target] = []
    seen: set[tuple[str, str]] = set()
    for target in targets:
        key = (target.name, str(target.db.get("architecture", "")))
        if key not in seen:
            unique.append(target)
            seen.add(key)
    return unique


def _expression_type(expression: Any) -> tuple[str, int] | None:
    if not isinstance(expression, Mapping):
        return None
    semantic_type = expression.get("type")
    if not isinstance(semantic_type, Mapping):
        return None
    kind, width = semantic_type.get("kind"), semantic_type.get("width")
    if not isinstance(kind, str) or type(width) is not int or width <= 0:
        return None
    return kind, width


def _resolve_local(
    expression: Any,
    locals_: Mapping[str, Mapping[str, Any]],
    used: set[str],
    active: frozenset[str] = frozenset(),
) -> Mapping[str, Any] | None:
    if not isinstance(expression, Mapping):
        return None
    if expression.get("op") != "local":
        return expression
    name = expression.get("name")
    if not isinstance(name, str) or name in active or name not in locals_:
        return None
    used.add(name)
    return _resolve_local(locals_[name], locals_, used, active | {name})


def _exact_leaf(
    expression: Any,
    locals_: Mapping[str, Mapping[str, Any]],
    used: set[str],
    operands: Mapping[str, Mapping[str, Any]],
) -> tuple[str, str, tuple[str, int]] | None:
    resolved = _resolve_local(expression, locals_, used)
    if resolved is None:
        return None
    operation, name = resolved.get("op"), resolved.get("name")
    semantic_type = _expression_type(resolved)
    operand = operands.get(name) if isinstance(name, str) else None
    if semantic_type is None or not isinstance(operand, Mapping):
        return None
    if (
        operation == "read"
        and operand.get("kind") == "reg"
        and operand.get("direction") in {"in", "inout"}
    ):
        return name, "reg", semantic_type
    if (
        operation == "immediate"
        and operand.get("kind") == "imm"
        and operand.get("direction") == "in"
    ):
        return name, "imm", semantic_type
    return None


def _instruction_primitive(
    instruction: Mapping[str, Any],
) -> tuple[str, int, tuple[str, ...], tuple[int, ...]] | None:
    """Return a generic operation only for an exact validated ArchSem shape.

    This intentionally does not inspect the mnemonic.  It recognizes one
    result, no non-register effects, and a root operation whose children are
    exactly the declared input operands.  Nested expressions are not promoted
    to their contained operation.
    """

    semantics = instruction.get("semantics")
    raw_operands = instruction.get("operands")
    if not isinstance(semantics, Mapping) or not isinstance(raw_operands, list):
        return None
    statements = semantics.get("statements")
    effects = semantics.get("effects")
    word_width = semantics.get("word_width")
    if (
        not isinstance(statements, list)
        or not statements
        or not isinstance(effects, Mapping)
        or type(word_width) is not int
        or word_width <= 0
    ):
        return None

    operands: dict[str, Mapping[str, Any]] = {}
    for operand in raw_operands:
        if not isinstance(operand, Mapping) or not isinstance(operand.get("name"), str):
            return None
        name = str(operand["name"])
        if name in operands:
            return None
        operands[name] = operand

    # Candidate equalities in this scheduler are pure register/value
    # operations.  Memory, state, stack, trap, and other effects need a richer
    # matching contract and therefore remain unassociated target facts.
    for effect_name, resources in effects.items():
        if effect_name not in {"reads", "writes"} and resources:
            return None
    reads, writes = effects.get("reads"), effects.get("writes")
    if (
        not isinstance(reads, list)
        or not isinstance(writes, list)
        or any(not isinstance(name, str) for name in (*reads, *writes))
    ):
        return None

    locals_: dict[str, Mapping[str, Any]] = {}
    for statement in statements[:-1]:
        if (
            not isinstance(statement, Mapping)
            or statement.get("op") != "let"
            or not isinstance(statement.get("name"), str)
            or not isinstance(statement.get("value"), Mapping)
            or statement["name"] in locals_
        ):
            return None
        locals_[str(statement["name"])] = statement["value"]
    write = statements[-1]
    if (
        not isinstance(write, Mapping)
        or write.get("op") != "write"
        or not isinstance(write.get("name"), str)
        or not isinstance(write.get("value"), Mapping)
    ):
        return None
    output_name = str(write["name"])
    output = operands.get(output_name)
    if (
        not isinstance(output, Mapping)
        or output.get("kind") != "reg"
        or output.get("direction") not in {"out", "inout"}
    ):
        return None
    definitions = [
        str(operand["name"])
        for operand in raw_operands
        if isinstance(operand, Mapping)
        and operand.get("kind") == "reg"
        and operand.get("direction") in {"out", "inout"}
    ]
    if definitions != [output_name] or sorted(map(str, writes)) != [output_name]:
        return None

    used_locals: set[str] = set()
    root = _resolve_local(write["value"], locals_, used_locals)
    if root is None:
        return None
    operation = root.get("op")
    if operation in {"read", "immediate"}:
        primitive = "COPY"
        children = [root]
    elif operation in _SEMANTIC_PRIMITIVES:
        primitive = _SEMANTIC_PRIMITIVES[str(operation)]
        if operation == "~":
            children = [root.get("value")]
        elif operation in {"zext", "sext", "trunc", "bitcast"}:
            children = [root.get("value")]
        else:
            children = [root.get("left"), root.get("right")]
    elif operation == ">>":
        left = _resolve_local(root.get("left"), locals_, used_locals)
        left_type = _expression_type(left)
        if left is None or left_type is None:
            return None
        primitive = "ASHR" if left_type[0] == "sint" else "LSHR"
        children = [root.get("left"), root.get("right")]
    else:
        return None

    root_type = _expression_type(root)
    leaves = [
        _exact_leaf(child, locals_, used_locals, operands) for child in children
    ]
    if root_type is None or root_type[1] != word_width or any(
        leaf is None for leaf in leaves
    ):
        return None
    exact_leaves = [leaf for leaf in leaves if leaf is not None]
    expected_inputs = [
        str(operand["name"])
        for operand in raw_operands
        if isinstance(operand, Mapping)
        and (
            operand.get("kind") == "imm"
            or (
                operand.get("kind") == "reg"
                and operand.get("direction") in {"in", "inout"}
            )
        )
    ]
    if [leaf[0] for leaf in exact_leaves] != expected_inputs:
        return None
    if used_locals != set(locals_):
        return None
    register_reads = [leaf[0] for leaf in exact_leaves if leaf[1] == "reg"]
    if sorted(map(str, reads)) != sorted(register_reads):
        return None

    # The generic ANF operation has no implicit conversions.  Except for the
    # shift count and explicit conversion primitives, every leaf must have the
    # exact result width.
    leaf_types = [leaf[2] for leaf in exact_leaves]
    if primitive in {"ZEXT", "SEXT"}:
        if len(leaf_types) != 1 or leaf_types[0][1] > root_type[1]:
            return None
    elif primitive == "TRUNC":
        if len(leaf_types) != 1 or leaf_types[0][1] < root_type[1]:
            return None
    elif primitive == "BITCAST":
        if len(leaf_types) != 1 or leaf_types[0][1] != root_type[1]:
            return None
    elif primitive in {"SHL", "LSHR", "ASHR"}:
        if len(leaf_types) != 2 or leaf_types[0][1] != root_type[1]:
            return None
    elif any(leaf_type[1] != root_type[1] for leaf_type in leaf_types):
        return None
    return (
        primitive,
        root_type[1],
        tuple(leaf[1] for leaf in exact_leaves),
        tuple(leaf[2][1] for leaf in exact_leaves),
    )


def _encoding_size(instruction: Mapping[str, Any]) -> tuple[int, bool]:
    width = instruction.get("width")
    if type(width) is int and width > 0 and width % 8 == 0:
        return width // 8, True
    return 0, False


def _c_string(value: str) -> str:
    encoded = value.encode("utf-8")
    pieces = ['"']
    for byte in encoded:
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


def _tables(context: Any) -> tuple[str, str, str, int, int, int]:
    targets = _selected_targets(context)
    target_rows = [
        f"    {{{_c_string(target.name)}, {target.address_width}u}},"
        for target in targets
    ]
    facts: list[_InstructionFact] = []
    candidates: list[_Candidate] = []
    for target_index, target in enumerate(targets):
        instructions = target.db.get("instructions", [])
        if not isinstance(instructions, list):
            continue
        ordered = sorted(
            (instruction for instruction in instructions if isinstance(instruction, Mapping)),
            key=lambda instruction: (
                str(instruction.get("id", "")),
                str(instruction.get("name", "")),
            ),
        )
        for ordinal, instruction in enumerate(ordered):
            if not isinstance(instruction, Mapping):
                continue
            name = str(instruction.get("name", instruction.get("id", "instruction")))
            encoding_bytes, encoding_bytes_known = _encoding_size(instruction)
            fact = _InstructionFact(
                target_index,
                target.name,
                ordinal,
                name,
                encoding_bytes,
                encoding_bytes_known,
            )
            facts.append(fact)
            association = _instruction_primitive(instruction)
            if association is not None:
                primitive, result_width, input_kinds, input_widths = association
                candidates.append(
                    _Candidate(
                        primitive,
                        result_width,
                        input_kinds,
                        input_widths,
                        fact,
                    )
                )
    candidates.sort(
        key=lambda candidate: (
            candidate.primitive,
            candidate.fact.encoding_bytes
            if candidate.fact.encoding_bytes_known
            else 0x7FFFFFFF,
            candidate.fact.target_name,
            candidate.fact.instruction,
            candidate.input_kinds,
            candidate.input_widths,
        )
    )
    fact_rows = [
        "    {%du, %du, %s, %du, %du},"
        % (
            fact.target,
            fact.ordinal,
            _c_string(fact.instruction),
            fact.encoding_bytes,
            int(fact.encoding_bytes_known),
        )
        for fact in facts
    ]
    candidate_rows = [
        "    {ARCHON_IR_PRIM_%s, %du, %du, %s, %s, %du, %du, "
        "%du, %du, %du, {%s}},"
        % (
            candidate.primitive,
            candidate.fact.target,
            candidate.fact.ordinal,
            _c_string(candidate.fact.target_name),
            _c_string(candidate.fact.instruction),
            candidate.fact.encoding_bytes,
            int(candidate.fact.encoding_bytes_known),
            candidate.result_width,
            len(candidate.input_kinds),
            sum(
                1 << index
                for index, kind in enumerate(candidate.input_kinds)
                if kind == "imm"
            ),
            ", ".join(
                f"{width}u"
                for width in (
                    *candidate.input_widths,
                    *(0 for _ in range(4 - len(candidate.input_widths))),
                )
            ),
        )
        for candidate in candidates
    ]
    return (
        "\n".join(target_rows or ['    {"", 0u},']),
        "\n".join(fact_rows or ['    {0u, 0u, "", 0u, 0u},']),
        "\n".join(
            candidate_rows
            or [
                '    {ARCHON_IR_PRIM_USER, 0u, 0u, "", "", 0u, 0u, '
                '0u, 0u, 0u, {0u, 0u, 0u, 0u}},'
            ]
        ),
        len(targets),
        len(facts),
        len(candidates),
    )


def emit(context: Any) -> dict[str, str]:
    """Return ``Scheduler.h`` and ``Scheduler.c`` for selected ArchDBs."""

    oracle_enabled = _oracle_enabled(context)
    oracle_include = '#include "Oracle.h"\n' if oracle_enabled else ""
    oracle_option_field = (
        "    const char *annotation_path;\n" if oracle_enabled else ""
    )
    oracle_documentation = (
        _ORACLE_DOCUMENTATION if oracle_enabled else ""
    )
    oracle_support = _ORACLE_SUPPORT if oracle_enabled else ""
    oracle_block_parameter = (
        ",\n        const struct scheduler_oracle_advice *oracle_advice"
        if oracle_enabled
        else ""
    )
    oracle_block_argument = ", oracle_advice" if oracle_enabled else ""
    oracle_impl_name = (
        "scheduler_emit_mir_impl"
        if oracle_enabled
        else "archon_scheduler_emit_mir"
    )
    oracle_impl_linkage = "static " if oracle_enabled else ""
    oracle_impl_parameter = (
        ",\n        const struct scheduler_oracle_advice *oracle_advice"
        if oracle_enabled
        else ""
    )
    oracle_wrapper = _ORACLE_WRAPPER if oracle_enabled else ""
    oracle_operation = (
        "        if (oracle_advice != NULL && oracle_advice->loaded) {\n"
        "            size_t fact = scheduler_oracle_fact_begin(\n"
        "                oracle_advice,\n"
        "                block_operation_base + index);\n"
        "            while (fact < oracle_advice->fact_count &&\n"
        "                    oracle_advice->facts[fact].operation ==\n"
        "                        block_operation_base + index) {\n"
        "                const struct scheduler_oracle_fact *row =\n"
        "                    &oracle_advice->facts[fact++];\n"
        "                if (row->kind == SCHEDULER_ORACLE_LATENCY) {\n"
        "                    operation_latency[index] = row->value.u64;\n"
        "                } else if (row->kind == SCHEDULER_ORACLE_PRIORITY) {\n"
        "                    advisory_priority[index] = row->value.i64;\n"
        "                } else if (row->kind ==\n"
        "                        SCHEDULER_ORACLE_DEPENDENCY) {\n"
        "                    size_t predecessor;\n"
        "                    if (row->predecessor < block_operation_base ||\n"
        "                            row->predecessor >=\n"
        "                                block_operation_base + index) {\n"
        "                        status = scheduler_fail(\n"
        "                            diagnostic, diagnostic_capacity,\n"
        "                            ARCHON_SCHEDULER_INVALID_IR,\n"
        "                            \"Oracle dependency must name an earlier \"\n"
        "                            \"operation in the same block\");\n"
        "                        goto done;\n"
        "                    }\n"
        "                    predecessor =\n"
        "                        row->predecessor - block_operation_base;\n"
        "                    dependencies[index * count + predecessor] |=\n"
        "                        MIR_DEPENDENCY_CONTROL;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        if oracle_enabled
        else ""
    )
    representation = _member(context, ("ir", "representation"))
    if (
        isinstance(representation, str)
        and representation.strip().lower().replace("_", "-") != "anf"
    ):
        return {
            "Scheduler.h": (
                _NON_ANF_HEADER.replace("@ORACLE_INCLUDE@", oracle_include)
                .replace("@ORACLE_OPTION_FIELD@", oracle_option_field)
            ),
            "Scheduler.c": (
                _NON_ANF_SOURCE.replace(
                    "@REPRESENTATION@", _c_string(representation)
                )
                .replace("@ORACLE_INCLUDE@", oracle_include)
                .replace(
                    "@ORACLE_DEFAULT_OPTION@",
                    '    options->annotation_path = "MIR.oracle.i";\n'
                    if oracle_enabled
                    else "",
                )
            ),
        }

    (
        targets,
        target_instructions,
        candidates,
        target_count,
        target_instruction_count,
        candidate_count,
    ) = _tables(context)
    source = (
        _SOURCE.replace("@TARGET_ROWS@", targets)
        .replace("@TARGET_INSTRUCTION_ROWS@", target_instructions)
        .replace("@CANDIDATE_ROWS@", candidates)
        .replace("@TARGET_COUNT@", str(target_count))
        .replace("@TARGET_INSTRUCTION_COUNT@", str(target_instruction_count))
        .replace("@CANDIDATE_COUNT@", str(candidate_count))
        .replace("@ORACLE_INCLUDE@", oracle_include)
        .replace("@ORACLE_SUPPORT@", oracle_support)
        .replace("@ORACLE_OPERATION@", oracle_operation)
        .replace("@ORACLE_BLOCK_PARAMETER@", oracle_block_parameter)
        .replace("@ORACLE_BLOCK_ARGUMENT@", oracle_block_argument)
        .replace("@ORACLE_IMPL_NAME@", oracle_impl_name)
        .replace("@ORACLE_IMPL_LINKAGE@", oracle_impl_linkage)
        .replace("@ORACLE_IMPL_PARAMETER@", oracle_impl_parameter)
        .replace("@ORACLE_WRAPPER@", oracle_wrapper)
        .replace(
            "@ORACLE_DEFAULT_OPTION@",
            '    options->annotation_path = "MIR.oracle.i";\n'
            if oracle_enabled
            else "",
        )
    )
    return {
        "Scheduler.h": (
            _HEADER.replace("@ORACLE_INCLUDE@", oracle_include)
            .replace("@ORACLE_OPTION_FIELD@", oracle_option_field)
            .replace("@ORACLE_DOCUMENTATION@", oracle_documentation)
        ),
        "Scheduler.c": source,
    }


_ORACLE_DOCUMENTATION = r"""
/*
 * Advisory schedule sidecars are separate from MIR.i and recognize only:
 *   scheduler.operation.<decimal-id>.latency             U64 (1..1000000)
 *   scheduler.operation.<decimal-id>.priority            I64
 *   scheduler.operation.<decimal-id>.depends_on.<id>     BOOL
 *
 * IDs are the numeric MIR_OP IDs.  Dependencies may only add an edge from an
 * earlier operation in the same block; they cannot erase data/effect/control
 * edges.  Recognized records are parsed once into a sorted table, with at most
 * 4096 recognized facts per invocation.  Unknown keys remain in the sidecar
 * and are ignored.
 *
 * annotation_path defaults to "MIR.oracle.i".  A missing sidecar is a no-op.
 */
"""


_ORACLE_SUPPORT = r"""
enum scheduler_oracle_fact_kind {
    SCHEDULER_ORACLE_LATENCY = 1,
    SCHEDULER_ORACLE_PRIORITY,
    SCHEDULER_ORACLE_DEPENDENCY
};

struct scheduler_oracle_fact {
    size_t operation;
    size_t predecessor;
    enum scheduler_oracle_fact_kind kind;
    union {
        uint64_t u64;
        int64_t i64;
    } value;
};

struct scheduler_oracle_advice {
    ArchonOracleAnnotations annotations;
    struct scheduler_oracle_fact *facts;
    size_t fact_count;
    int loaded;
};

static void scheduler_oracle_dispose(
        struct scheduler_oracle_advice *advice) {
    if (advice == NULL)
        return;
    free(advice->facts);
    archon_oracle_annotations_dispose(&advice->annotations);
    memset(advice, 0, sizeof *advice);
}

static int scheduler_oracle_decimal(
        const char **cursor, size_t *value) {
    const char *text;
    size_t result = 0u;
    if (cursor == NULL || *cursor == NULL || value == NULL)
        return 0;
    text = *cursor;
    if (*text < '0' || *text > '9')
        return 0;
    do {
        unsigned digit = (unsigned)(*text - '0');
        if (result > (SIZE_MAX - digit) / 10u)
            return 0;
        result = result * 10u + digit;
        ++text;
    } while (*text >= '0' && *text <= '9');
    *cursor = text;
    *value = result;
    return 1;
}

static int scheduler_oracle_fact_compare(
        const void *left_pointer, const void *right_pointer) {
    const struct scheduler_oracle_fact *left =
        (const struct scheduler_oracle_fact *)left_pointer;
    const struct scheduler_oracle_fact *right =
        (const struct scheduler_oracle_fact *)right_pointer;
    if (left->operation < right->operation)
        return -1;
    if (left->operation > right->operation)
        return 1;
    if (left->kind < right->kind)
        return -1;
    if (left->kind > right->kind)
        return 1;
    if (left->predecessor < right->predecessor)
        return -1;
    if (left->predecessor > right->predecessor)
        return 1;
    return 0;
}

static size_t scheduler_oracle_fact_begin(
        const struct scheduler_oracle_advice *advice,
        size_t operation) {
    size_t low = 0u;
    size_t high = advice != NULL ? advice->fact_count : 0u;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (advice->facts[middle].operation < operation)
            low = middle + 1u;
        else
            high = middle;
    }
    return low;
}

static enum archon_scheduler_status scheduler_oracle_add_fact(
        struct scheduler_oracle_advice *advice,
        const struct scheduler_oracle_fact *fact,
        char *diagnostic, size_t diagnostic_capacity) {
    enum { SCHEDULER_ORACLE_MAX_RECOGNIZED_FACTS = 4096 };
    if (advice->fact_count >= SCHEDULER_ORACLE_MAX_RECOGNIZED_FACTS)
        return scheduler_fail(
            diagnostic, diagnostic_capacity, ARCHON_SCHEDULER_LIMIT,
            "scheduler Oracle sidecar exceeds the recognized-fact limit");
    advice->facts[advice->fact_count++] = *fact;
    return ARCHON_SCHEDULER_OK;
}

static enum archon_scheduler_status scheduler_oracle_parse_facts(
        struct scheduler_oracle_advice *advice,
        char *diagnostic, size_t diagnostic_capacity) {
    static const char prefix[] = "scheduler.operation.";
    size_t annotation;
    if (advice->annotations.count > SIZE_MAX / sizeof *advice->facts)
        return scheduler_fail(
            diagnostic, diagnostic_capacity, ARCHON_SCHEDULER_LIMIT,
            "scheduler Oracle fact table is too large");
    advice->facts = (struct scheduler_oracle_fact *)calloc(
        advice->annotations.count != 0u
            ? advice->annotations.count : 1u,
        sizeof *advice->facts);
    if (advice->facts == NULL)
        return scheduler_fail(
            diagnostic, diagnostic_capacity,
            ARCHON_SCHEDULER_OUT_OF_MEMORY,
            "out of memory while parsing scheduler Oracle advice");
    for (annotation = 0u;
         annotation < advice->annotations.count; ++annotation) {
        const ArchonOracleAnnotation *entry =
            &advice->annotations.entries[annotation];
        const char *cursor;
        struct scheduler_oracle_fact fact;
        enum archon_scheduler_status status;
        int recognized = 0;
        memset(&fact, 0, sizeof fact);
        if (strncmp(entry->key, prefix, sizeof prefix - 1u) != 0)
            continue;
        cursor = entry->key + sizeof prefix - 1u;
        if (!scheduler_oracle_decimal(&cursor, &fact.operation))
            continue;
        if (strcmp(cursor, ".latency") == 0) {
            recognized = 1;
            if (entry->kind != ARCHON_ORACLE_ANNOTATION_U64)
                return scheduler_fail(
                    diagnostic, diagnostic_capacity,
                    ARCHON_SCHEDULER_INVALID_IR,
                    "scheduler latency annotation has the wrong type");
            if (entry->value.u64 == 0u ||
                    entry->value.u64 > UINT64_C(1000000))
                return scheduler_fail(
                    diagnostic, diagnostic_capacity,
                    ARCHON_SCHEDULER_LIMIT,
                    "scheduler operation latency is outside "
                    "the validated range");
            fact.kind = SCHEDULER_ORACLE_LATENCY;
            fact.value.u64 = entry->value.u64;
        } else if (strcmp(cursor, ".priority") == 0) {
            recognized = 1;
            if (entry->kind != ARCHON_ORACLE_ANNOTATION_I64)
                return scheduler_fail(
                    diagnostic, diagnostic_capacity,
                    ARCHON_SCHEDULER_INVALID_IR,
                    "scheduler priority annotation has the wrong type");
            fact.kind = SCHEDULER_ORACLE_PRIORITY;
            fact.value.i64 = entry->value.i64;
        } else if (strncmp(cursor, ".depends_on.", 12u) == 0) {
            const char *predecessor = cursor + 12u;
            recognized = scheduler_oracle_decimal(
                &predecessor, &fact.predecessor) &&
                *predecessor == '\0';
            if (!recognized)
                continue;
            if (entry->kind != ARCHON_ORACLE_ANNOTATION_BOOL)
                return scheduler_fail(
                    diagnostic, diagnostic_capacity,
                    ARCHON_SCHEDULER_INVALID_IR,
                    "scheduler dependency annotation has the wrong type");
            if (!entry->value.boolean)
                continue;
            fact.kind = SCHEDULER_ORACLE_DEPENDENCY;
        }
        if (!recognized)
            continue;
        status = scheduler_oracle_add_fact(
            advice, &fact, diagnostic, diagnostic_capacity);
        if (status != ARCHON_SCHEDULER_OK)
            return status;
    }
    qsort(
        advice->facts, advice->fact_count, sizeof *advice->facts,
        scheduler_oracle_fact_compare);
    return ARCHON_SCHEDULER_OK;
}

static enum archon_scheduler_status scheduler_oracle_load(
        const struct archon_scheduler_options *options,
        struct scheduler_oracle_advice *advice,
        char *diagnostic, size_t diagnostic_capacity) {
    const char *path;
    ArchonOracleDiagnostic oracle_diagnostic;
    ArchonOracleStatus status;
    if (options == NULL || advice == NULL)
        return scheduler_fail(
            diagnostic, diagnostic_capacity,
            ARCHON_SCHEDULER_INVALID_ARGUMENT,
            "invalid Oracle scheduler arguments");
    memset(advice, 0, sizeof *advice);
    archon_oracle_annotations_init(&advice->annotations);
    archon_oracle_diagnostic_init(&oracle_diagnostic);
    path = options->annotation_path != NULL &&
            options->annotation_path[0] != '\0'
        ? options->annotation_path : "MIR.oracle.i";
    status = archon_oracle_annotations_load(
        path, &advice->annotations, &oracle_diagnostic);
    if (status == ARCHON_ORACLE_STATUS_NOT_FOUND) {
        archon_oracle_diagnostic_init(&oracle_diagnostic);
        return ARCHON_SCHEDULER_OK;
    }
    if (status != ARCHON_ORACLE_STATUS_OK) {
        scheduler_oracle_dispose(advice);
        return scheduler_fail(
            diagnostic, diagnostic_capacity,
            ARCHON_SCHEDULER_INVALID_IR,
            "invalid scheduler Oracle sidecar: %s%s%s",
            archon_oracle_status_string(status),
            oracle_diagnostic.message[0] != '\0' ? ": " : "",
            oracle_diagnostic.message);
    }
    if (advice->annotations.stage != ARCHON_ORACLE_STAGE_SCHEDULE) {
        scheduler_oracle_dispose(advice);
        return scheduler_fail(
            diagnostic, diagnostic_capacity,
            ARCHON_SCHEDULER_INVALID_IR,
            "scheduler Oracle sidecar has the wrong stage");
    }
    {
        enum archon_scheduler_status parse_status =
            scheduler_oracle_parse_facts(
                advice, diagnostic, diagnostic_capacity);
        if (parse_status != ARCHON_SCHEDULER_OK) {
            scheduler_oracle_dispose(advice);
            return parse_status;
        }
    }
    advice->loaded = 1;
    return ARCHON_SCHEDULER_OK;
}
"""


_ORACLE_WRAPPER = r"""
enum archon_scheduler_status archon_scheduler_emit_mir(
        FILE *output, const struct archon_ir_program *program,
        const struct archon_scheduler_options *requested_options,
        char *diagnostic, size_t diagnostic_capacity) {
    struct archon_scheduler_options defaults;
    const struct archon_scheduler_options *options = requested_options;
    struct scheduler_oracle_advice advice;
    enum archon_scheduler_status status;
    if (output == NULL || program == NULL)
        return scheduler_fail(
            diagnostic, diagnostic_capacity,
            ARCHON_SCHEDULER_INVALID_ARGUMENT,
            "null scheduler argument");
    if (options == NULL) {
        archon_scheduler_default_options(&defaults);
        options = &defaults;
    }
    if (options->maximum_block_operations == 0u)
        return scheduler_fail(
            diagnostic, diagnostic_capacity,
            ARCHON_SCHEDULER_INVALID_ARGUMENT,
            "invalid scheduler options");
    memset(&advice, 0, sizeof advice);
    status = scheduler_oracle_load(
        options, &advice, diagnostic, diagnostic_capacity);
    if (status == ARCHON_SCHEDULER_OK)
        status = scheduler_emit_mir_impl(
            output, program, options, &advice,
            diagnostic, diagnostic_capacity);
    scheduler_oracle_dispose(&advice);
    return status;
}
"""


_NON_ANF_HEADER = r'''/* Generated by Archon: conservative scheduler boundary.
 *
 * This representation does not expose the typed ANF view consumed by the
 * legacy in-process scheduler API.  The source remains a strict-C99,
 * independently compilable stage and reports the unsupported lowering
 * explicitly instead of interpreting an unrelated IR.h layout as ANF.
 */
#ifndef ARCHON_GENERATED_SCHEDULER_H
#define ARCHON_GENERATED_SCHEDULER_H

@ORACLE_INCLUDE@
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum archon_scheduler_status {
    ARCHON_SCHEDULER_OK = 0,
    ARCHON_SCHEDULER_INVALID_ARGUMENT,
    ARCHON_SCHEDULER_INVALID_IR,
    ARCHON_SCHEDULER_OUT_OF_MEMORY,
    ARCHON_SCHEDULER_LIMIT,
    ARCHON_SCHEDULER_IO_ERROR,
    ARCHON_SCHEDULER_CYCLE,
    ARCHON_SCHEDULER_UNSUPPORTED
};

struct archon_scheduler_options {
    size_t maximum_block_operations;
@ORACLE_OPTION_FIELD@
};

void archon_scheduler_default_options(
    struct archon_scheduler_options *options);
enum archon_scheduler_status archon_scheduler_emit_embedded(
    FILE *output, const struct archon_scheduler_options *options,
    char *diagnostic, size_t diagnostic_capacity);
const char *archon_scheduler_status_name(
    enum archon_scheduler_status status);

#ifdef __cplusplus
}
#endif
#endif
'''


_NON_ANF_SOURCE = r'''/* Generated by Archon.  This file is strict ISO C99. */
#include "Scheduler.h"
@ORACLE_INCLUDE@

#include <stdio.h>

static const char scheduler_representation[] = @REPRESENTATION@;

void archon_scheduler_default_options(
        struct archon_scheduler_options *options) {
    if (options != NULL) {
        options->maximum_block_operations = 4096u;
@ORACLE_DEFAULT_OPTION@
    }
}

enum archon_scheduler_status archon_scheduler_emit_embedded(
        FILE *output, const struct archon_scheduler_options *options,
        char *diagnostic, size_t diagnostic_capacity) {
    (void)output;
    (void)options;
    if (diagnostic != NULL && diagnostic_capacity != 0u) {
        (void)snprintf(
            diagnostic, diagnostic_capacity,
            "representation '%s' has no semantics-preserving ANF lowering",
            scheduler_representation);
        diagnostic[diagnostic_capacity - 1u] = '\0';
    }
    return ARCHON_SCHEDULER_UNSUPPORTED;
}

const char *archon_scheduler_status_name(
        enum archon_scheduler_status status) {
    static const char *const names[] = {
        "ok", "invalid argument", "invalid IR", "out of memory",
        "limit exceeded", "I/O error", "dependency cycle", "unsupported"
    };
    size_t index = (size_t)status;
    return index < sizeof names / sizeof names[0] ? names[index]
                                                  : "unknown status";
}
'''


_HEADER = r'''/* Generated by Archon: deterministic ANF list scheduler.
 *
 * MachineIR (MIR.i) X-macro schema, version 2
 * --------------------------------------------
 * Each line is one macro invocation without a trailing semicolon.  Consumers
 * define the macros they need and include MIR.i.  Numeric ids are stable:
 * functions, blocks and values use source order; operation ids use source
 * order across the program, while the MIR_OP slot records scheduled order.
 *
 * MIR_SCHEMA(version)
 * MIR_PROGRAM_BEGIN(name)
 * MIR_TARGET(id, name, address_width)
 * MIR_TARGET_INSTRUCTION(target, ordinal, name,
 *        encoding_bytes, encoding_bytes_known,
 *        latency_cycles, latency_known,
 *        reciprocal_throughput_milli, throughput_known,
 *        resource_name, resource_known)
 * MIR_FUNCTION_BEGIN(id, key, name, result_type, result_width)
 * MIR_VALUE(function, id, key, name, type, width, is_parameter)
 * MIR_BLOCK_BEGIN(function, block, key, name)
 * MIR_CANDIDATE_SET_BEGIN(set, opcode, count)
 * MIR_CANDIDATE(set, ordinal, target, target_instruction, target_name,
 *        instruction, encoding_bytes, encoding_bytes_known,
 *        latency_cycles, latency_known,
 *        reciprocal_throughput_milli, throughput_known,
 *        resource_name, resource_known)
 * MIR_CANDIDATE_SET_END(set)
 * MIR_OP(id, function, block, slot, result, opcode,
 *        unit_ordering_priority, effects, candidate_set)
 * MIR_INPUT_VALUE(op, index, value)
 * MIR_INPUT_INTEGER(op, index, type, width, uint64_constant)
 * MIR_INPUT_BOOLEAN(op, index, value)
 * MIR_INPUT_SYMBOL(op, index, string)
 * MIR_DEPENDENCY(op, predecessor, dependency_flags)
 * MIR_CALL_TARGET(op, function_key)
 * MIR_BRANCH_TARGETS(op, true_block_key, false_block_key)
 * MIR_JUMP_TARGET(op, block_key)
 * MIR_BLOCK_END(function, block)
 * MIR_FUNCTION_END(function)
 * MIR_PROGRAM_END()
 *
 * result is MIR_NO_VALUE for effect-only operations.  Opcodes are
 * MIR_OPCODE_*, types are MIR_TYPE_*, dependency flags are MIR_DEP_DATA,
 * MIR_DEP_EFFECT and MIR_DEP_CONTROL, and effects are MIR_EFFECT_* bitwise
 * expressions.  Every selected target instruction is retained as a target
 * fact.  It enters a generic candidate set only when its complete ArchSem
 * shape proves that exact association and its operand kinds/widths match the
 * ANF operation; names and mnemonics are never used.
 *
 * The *_known fields distinguish facts from absent schema data.  Schema-v1
 * ArchDB gives encoding width, so encoding_bytes_known is normally one.
 * It does not define latency, reciprocal throughput, or execution resources;
 * those values are emitted as zero with their corresponding *_known field
 * zero.  Zero is not silently interpreted as a measured cost.
 *
 * The list scheduler uses a target-independent unit weight for each operation.
 * unit_ordering_priority is the resulting longest dependency-chain length. It
 * is exposed solely to explain deterministic ordering and is not target
 * latency, throughput, or cost.
 */
#ifndef ARCHON_GENERATED_SCHEDULER_H
#define ARCHON_GENERATED_SCHEDULER_H

#include "IR.h"
@ORACLE_INCLUDE@

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum archon_scheduler_status {
    ARCHON_SCHEDULER_OK = 0,
    ARCHON_SCHEDULER_INVALID_ARGUMENT,
    ARCHON_SCHEDULER_INVALID_IR,
    ARCHON_SCHEDULER_OUT_OF_MEMORY,
    ARCHON_SCHEDULER_LIMIT,
    ARCHON_SCHEDULER_IO_ERROR,
    ARCHON_SCHEDULER_CYCLE
};

struct archon_scheduler_options {
    size_t maximum_block_operations;
@ORACLE_OPTION_FIELD@
};

@ORACLE_DOCUMENTATION@

void archon_scheduler_default_options(
    struct archon_scheduler_options *options);
enum archon_scheduler_status archon_scheduler_emit_mir(
    FILE *output, const struct archon_ir_program *program,
    const struct archon_scheduler_options *options,
    char *diagnostic, size_t diagnostic_capacity);
enum archon_scheduler_status archon_scheduler_emit_embedded(
    FILE *output, const struct archon_scheduler_options *options,
    char *diagnostic, size_t diagnostic_capacity);
const char *archon_scheduler_status_name(
    enum archon_scheduler_status status);

#ifdef __cplusplus
}
#endif
#endif
'''


_SOURCE = r'''/* Generated by Archon.  This file is strict ISO C99. */
#include "Scheduler.h"
@ORACLE_INCLUDE@

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    MIR_DEPENDENCY_DATA = 1u,
    MIR_DEPENDENCY_EFFECT = 2u,
    MIR_DEPENDENCY_CONTROL = 4u
};

struct archon_scheduler_target {
    const char *name;
    unsigned address_width;
};

struct archon_scheduler_target_instruction {
    unsigned target;
    unsigned ordinal;
    const char *instruction;
    unsigned encoding_bytes;
    unsigned encoding_bytes_known;
};

struct archon_scheduler_candidate {
    enum archon_ir_primitive primitive;
    unsigned target;
    unsigned target_instruction;
    const char *target_name;
    const char *instruction;
    unsigned encoding_bytes;
    unsigned encoding_bytes_known;
    unsigned result_width;
    unsigned argument_count;
    unsigned immediate_mask;
    unsigned argument_width[4];
};

static const struct archon_scheduler_target scheduler_targets[] = {
@TARGET_ROWS@
};

static const struct archon_scheduler_target_instruction
scheduler_target_instructions[] = {
@TARGET_INSTRUCTION_ROWS@
};

static const struct archon_scheduler_candidate scheduler_candidates[] = {
@CANDIDATE_ROWS@
};

static const size_t scheduler_target_count = @TARGET_COUNT@u;
static const size_t scheduler_target_instruction_count =
    @TARGET_INSTRUCTION_COUNT@u;
static const size_t scheduler_candidate_count = @CANDIDATE_COUNT@u;

static enum archon_scheduler_status scheduler_fail(
        char *diagnostic, size_t capacity,
        enum archon_scheduler_status status, const char *format, ...) {
    if (diagnostic && capacity) {
        va_list arguments;
        va_start(arguments, format);
        (void)vsnprintf(diagnostic, capacity, format, arguments);
        va_end(arguments);
        diagnostic[capacity - 1u] = '\0';
    }
    return status;
}

@ORACLE_SUPPORT@

static int scheduler_put_string(FILE *output, const char *value) {
    const unsigned char *cursor = (const unsigned char *)value;
    if (fputc('"', output) == EOF)
        return 0;
    while (*cursor) {
        unsigned byte = *cursor++;
        if (byte == '"' || byte == '\\') {
            if (fputc('\\', output) == EOF ||
                    fputc((int)byte, output) == EOF)
                return 0;
        } else if (byte >= 0x20u && byte <= 0x7eu) {
            if (fputc((int)byte, output) == EOF)
                return 0;
        } else if (fprintf(output, "\\%03o", byte) < 0) {
            return 0;
        }
    }
    return fputc('"', output) != EOF;
}

static const char *scheduler_type_token(enum archon_ir_type_kind kind) {
    static const char *const names[] = {
        "MIR_TYPE_VOID", "MIR_TYPE_BOOL", "MIR_TYPE_I8", "MIR_TYPE_I16",
        "MIR_TYPE_I32", "MIR_TYPE_I64", "MIR_TYPE_U8", "MIR_TYPE_U16",
        "MIR_TYPE_U32", "MIR_TYPE_U64", "MIR_TYPE_BITS", "MIR_TYPE_PTR",
        "MIR_TYPE_TOKEN"
    };
    size_t index = (size_t)kind;
    return index < sizeof names / sizeof names[0] ? names[index]
                                                  : "MIR_TYPE_INVALID";
}

static const char *scheduler_primitive_token(
        enum archon_ir_primitive primitive) {
    static const char *const names[] = {
        "MIR_OPCODE_COPY", "MIR_OPCODE_ADD", "MIR_OPCODE_SUB",
        "MIR_OPCODE_MUL", "MIR_OPCODE_UDIV", "MIR_OPCODE_SDIV",
        "MIR_OPCODE_UREM", "MIR_OPCODE_SREM", "MIR_OPCODE_NEG",
        "MIR_OPCODE_AND", "MIR_OPCODE_OR", "MIR_OPCODE_XOR",
        "MIR_OPCODE_NOT", "MIR_OPCODE_SHL", "MIR_OPCODE_LSHR",
        "MIR_OPCODE_ASHR", "MIR_OPCODE_EQ", "MIR_OPCODE_NE",
        "MIR_OPCODE_ULT", "MIR_OPCODE_ULE", "MIR_OPCODE_SLT",
        "MIR_OPCODE_SLE", "MIR_OPCODE_SELECT", "MIR_OPCODE_ZEXT",
        "MIR_OPCODE_SEXT", "MIR_OPCODE_TRUNC", "MIR_OPCODE_BITCAST",
        "MIR_OPCODE_LOAD", "MIR_OPCODE_STORE", "MIR_OPCODE_FENCE",
        "MIR_OPCODE_TRAP", "MIR_OPCODE_USER"
    };
    size_t index = (size_t)primitive;
    return index < sizeof names / sizeof names[0] ? names[index]
                                                  : "MIR_OPCODE_INVALID";
}

static const char *scheduler_operation_token(
        const struct archon_ir_operation *operation) {
    switch (operation->kind) {
    case ARCHON_IR_OP_CALL:
        return "MIR_OPCODE_CALL";
    case ARCHON_IR_OP_BRANCH:
        return "MIR_OPCODE_BRANCH";
    case ARCHON_IR_OP_JUMP:
        return "MIR_OPCODE_JUMP";
    case ARCHON_IR_OP_RETURN:
        return "MIR_OPCODE_RETURN";
    default:
        return scheduler_primitive_token(operation->primitive);
    }
}

static int scheduler_write_effects(FILE *output, unsigned effects) {
    static const struct {
        unsigned bit;
        const char *name;
    } rows[] = {
        {ARCHON_IR_EFFECT_READ, "MIR_EFFECT_READ"},
        {ARCHON_IR_EFFECT_WRITE, "MIR_EFFECT_WRITE"},
        {ARCHON_IR_EFFECT_CALL, "MIR_EFFECT_CALL"},
        {ARCHON_IR_EFFECT_CONTROL, "MIR_EFFECT_CONTROL"},
        {ARCHON_IR_EFFECT_VOLATILE, "MIR_EFFECT_VOLATILE"},
        {ARCHON_IR_EFFECT_TRAP, "MIR_EFFECT_TRAP"},
        {ARCHON_IR_EFFECT_IO, "MIR_EFFECT_IO"}
    };
    size_t index;
    int first = 1;
    if (!effects)
        return fputs("MIR_EFFECT_NONE", output) >= 0;
    for (index = 0; index < sizeof rows / sizeof rows[0]; ++index)
        if (effects & rows[index].bit) {
            if (!first && fputs(" | ", output) < 0)
                return 0;
            if (fputs(rows[index].name, output) < 0)
                return 0;
            first = 0;
        }
    return 1;
}

static int scheduler_write_dependencies(FILE *output, unsigned flags) {
    int first = 1;
    if (flags & MIR_DEPENDENCY_DATA) {
        if (fputs("MIR_DEP_DATA", output) < 0)
            return 0;
        first = 0;
    }
    if (flags & MIR_DEPENDENCY_EFFECT) {
        if (!first && fputs(" | ", output) < 0)
            return 0;
        if (fputs("MIR_DEP_EFFECT", output) < 0)
            return 0;
        first = 0;
    }
    if (flags & MIR_DEPENDENCY_CONTROL) {
        if (!first && fputs(" | ", output) < 0)
            return 0;
        if (fputs("MIR_DEP_CONTROL", output) < 0)
            return 0;
    }
    return 1;
}

static int scheduler_is_terminator(
        const struct archon_ir_operation *operation) {
    return operation->kind == ARCHON_IR_OP_BRANCH ||
           operation->kind == ARCHON_IR_OP_JUMP ||
           operation->kind == ARCHON_IR_OP_RETURN;
}

static int scheduler_candidate_matches(
        const struct archon_scheduler_candidate *candidate,
        const struct archon_ir_operation *operation) {
    size_t index;
    if (operation->kind != ARCHON_IR_OP_LET ||
            operation->effects != ARCHON_IR_EFFECT_NONE ||
            candidate->primitive != operation->primitive ||
            candidate->result_width != operation->result_type.width ||
            candidate->argument_count != operation->argument_count)
        return 0;
    for (index = 0; index < operation->argument_count; ++index) {
        const struct archon_ir_atom *atom = &operation->arguments[index];
        int immediate = (candidate->immediate_mask & (1u << index)) != 0u;
        if ((immediate && atom->kind != ARCHON_IR_ATOM_INTEGER) ||
                (!immediate && atom->kind != ARCHON_IR_ATOM_VALUE) ||
                atom->type.width != candidate->argument_width[index])
            return 0;
    }
    return 1;
}

static size_t scheduler_operation_candidate_count(
        const struct archon_ir_operation *operation) {
    size_t count = 0u;
    size_t index;
    for (index = 0; index < scheduler_candidate_count; ++index) {
        const struct archon_scheduler_candidate *candidate =
            &scheduler_candidates[index];
        if (scheduler_candidate_matches(candidate, operation))
            ++count;
    }
    return count;
}

static int scheduler_emit_candidate_set(
        FILE *output, size_t set_id,
        const struct archon_ir_operation *operation,
        size_t count) {
    size_t index, ordinal = 0u;
    if (fprintf(output, "MIR_CANDIDATE_SET_BEGIN(%zu, %s, %zu)\n",
                set_id, scheduler_operation_token(operation), count) < 0)
        return 0;
    for (index = 0; index < scheduler_candidate_count; ++index) {
        const struct archon_scheduler_candidate *candidate =
            &scheduler_candidates[index];
        if (!scheduler_candidate_matches(candidate, operation))
            continue;
        if (fprintf(output, "MIR_CANDIDATE(%zu, %zu, %u, %u, ",
                    set_id, ordinal++, candidate->target,
                    candidate->target_instruction) < 0 ||
                !scheduler_put_string(output, candidate->target_name) ||
                fputs(", ", output) < 0 ||
                !scheduler_put_string(output, candidate->instruction) ||
                fprintf(output,
                    ", %u, %u, 0, 0, 0, 0, \"\", 0)\n",
                    candidate->encoding_bytes,
                    candidate->encoding_bytes_known) < 0)
            return 0;
    }
    return fprintf(output, "MIR_CANDIDATE_SET_END(%zu)\n", set_id) >= 0;
}

static int scheduler_emit_atom(FILE *output, size_t operation_id,
        size_t argument_index, const struct archon_ir_atom *atom) {
    switch (atom->kind) {
    case ARCHON_IR_ATOM_VALUE:
        return fprintf(output, "MIR_INPUT_VALUE(%zu, %zu, %" PRIu32 ")\n",
                       operation_id, argument_index,
                       atom->as.value) >= 0;
    case ARCHON_IR_ATOM_INTEGER:
        return fprintf(output,
            "MIR_INPUT_INTEGER(%zu, %zu, %s, %u, "
            "UINT64_C(0x%" PRIx64 "))\n",
            operation_id, argument_index,
            scheduler_type_token(atom->type.kind), atom->type.width,
            atom->as.integer) >= 0;
    case ARCHON_IR_ATOM_BOOLEAN:
        return fprintf(output, "MIR_INPUT_BOOLEAN(%zu, %zu, %d)\n",
                       operation_id, argument_index,
                       atom->as.boolean != 0) >= 0;
    case ARCHON_IR_ATOM_SYMBOL:
        if (fprintf(output, "MIR_INPUT_SYMBOL(%zu, %zu, ",
                    operation_id, argument_index) < 0 ||
                !scheduler_put_string(output, atom->as.symbol))
            return 0;
        return fputs(")\n", output) >= 0;
    default:
        return 0;
    }
}

static int scheduler_emit_operation(FILE *output,
        const struct archon_ir_operation *operation,
        size_t operation_id, size_t function_id, size_t block_id,
        size_t slot, const unsigned char *dependencies,
        size_t operation_count, size_t block_operation_base,
        uint64_t ordering_priority) {
    size_t index;
    if (!scheduler_emit_candidate_set(output, operation_id, operation,
            scheduler_operation_candidate_count(operation)))
        return 0;
    if (fprintf(output, "MIR_OP(%zu, %zu, %zu, %zu, ",
                operation_id, function_id, block_id, slot) < 0)
        return 0;
    if (operation->result == ARCHON_IR_NO_VALUE) {
        if (fputs("MIR_NO_VALUE", output) < 0)
            return 0;
    } else if (fprintf(output, "%" PRIu32, operation->result) < 0) {
        return 0;
    }
    if (fprintf(output, ", %s, %" PRIu64 ", ",
                scheduler_operation_token(operation), ordering_priority) < 0 ||
            !scheduler_write_effects(output, operation->effects) ||
            fprintf(output, ", %zu)\n", operation_id) < 0)
        return 0;
    for (index = 0; index < operation->argument_count; ++index)
        if (!scheduler_emit_atom(output, operation_id, index,
                                 &operation->arguments[index]))
            return 0;
    for (index = 0; index < operation_count; ++index) {
        unsigned flags = dependencies[index];
        if (!flags)
            continue;
        if (fprintf(output, "MIR_DEPENDENCY(%zu, %zu, ",
                    operation_id, block_operation_base + index) < 0 ||
                !scheduler_write_dependencies(output, flags) ||
                fputs(")\n", output) < 0)
            return 0;
    }
    if (operation->kind == ARCHON_IR_OP_CALL) {
        if (fprintf(output, "MIR_CALL_TARGET(%zu, ", operation_id) < 0 ||
                !scheduler_put_string(output, operation->callee) ||
                fputs(")\n", output) < 0)
            return 0;
    } else if (operation->kind == ARCHON_IR_OP_BRANCH) {
        if (fprintf(output, "MIR_BRANCH_TARGETS(%zu, ", operation_id) < 0 ||
                !scheduler_put_string(output, operation->true_target) ||
                fputs(", ", output) < 0 ||
                !scheduler_put_string(output, operation->false_target) ||
                fputs(")\n", output) < 0)
            return 0;
    } else if (operation->kind == ARCHON_IR_OP_JUMP) {
        if (fprintf(output, "MIR_JUMP_TARGET(%zu, ", operation_id) < 0 ||
                !scheduler_put_string(output, operation->true_target) ||
                fputs(")\n", output) < 0)
            return 0;
    }
    return 1;
}

static enum archon_scheduler_status scheduler_schedule_block(
        FILE *output, const struct archon_ir_function *function,
        size_t function_id, const struct archon_ir_block *block,
        size_t block_id, size_t block_operation_base
        @ORACLE_BLOCK_PARAMETER@,
        const struct archon_scheduler_options *options,
        char *diagnostic, size_t diagnostic_capacity) {
    size_t count = block->operation_count;
    unsigned char *dependencies = NULL;
    unsigned char *scheduled = NULL;
    uint64_t *critical_path = NULL;
    uint64_t *operation_latency = NULL;
    int64_t *advisory_priority = NULL;
    size_t index, slot, last_effect = ARCHON_IR_NO_INDEX;
    enum archon_scheduler_status status = ARCHON_SCHEDULER_OK;

    if (count > options->maximum_block_operations)
        return scheduler_fail(
            diagnostic, diagnostic_capacity, ARCHON_SCHEDULER_LIMIT,
            "block '%s' has %lu operations (limit %lu)", block->key,
            (unsigned long)count,
            (unsigned long)options->maximum_block_operations);
    if (count && count > SIZE_MAX / count)
        return scheduler_fail(diagnostic, diagnostic_capacity,
                              ARCHON_SCHEDULER_LIMIT,
                              "dependency matrix is too large");
    dependencies = (unsigned char *)calloc(
        count ? count * count : 1u, sizeof *dependencies);
    scheduled = (unsigned char *)calloc(count ? count : 1u,
                                         sizeof *scheduled);
    critical_path = (uint64_t *)calloc(count ? count : 1u,
                                        sizeof *critical_path);
    operation_latency = (uint64_t *)malloc(
        (count ? count : 1u) * sizeof *operation_latency);
    advisory_priority = (int64_t *)calloc(
        count ? count : 1u, sizeof *advisory_priority);
    if (!dependencies || !scheduled || !critical_path ||
            !operation_latency || !advisory_priority) {
        status = scheduler_fail(diagnostic, diagnostic_capacity,
                                ARCHON_SCHEDULER_OUT_OF_MEMORY,
                                "out of memory while scheduling");
        goto done;
    }
    for (index = 0u; index < count; ++index)
        operation_latency[index] = UINT64_C(1);

    for (index = 0; index < count; ++index) {
        const struct archon_ir_operation *operation =
            &block->operations[index];
        size_t argument_index;
        for (argument_index = 0;
             argument_index < operation->argument_count;
             ++argument_index) {
            const struct archon_ir_atom *atom =
                &operation->arguments[argument_index];
            if (atom->kind == ARCHON_IR_ATOM_VALUE) {
                const struct archon_ir_value *value =
                    &function->values[atom->as.value];
                if (!value->is_parameter && value->block == block_id &&
                        value->operation < index)
                    dependencies[index * count + value->operation] |=
                        MIR_DEPENDENCY_DATA;
            }
        }
        if (operation->effects != ARCHON_IR_EFFECT_NONE) {
            if (last_effect != ARCHON_IR_NO_INDEX)
                dependencies[index * count + last_effect] |=
                    MIR_DEPENDENCY_EFFECT;
            last_effect = index;
        }
        if (scheduler_is_terminator(operation)) {
            size_t predecessor;
            for (predecessor = 0; predecessor < index; ++predecessor)
                dependencies[index * count + predecessor] |=
                    MIR_DEPENDENCY_CONTROL;
        }
@ORACLE_OPERATION@
    }

    for (index = count; index-- > 0u;) {
        uint64_t longest_user = 0u;
        size_t user;
        for (user = index + 1u; user < count; ++user)
            if (dependencies[user * count + index] &&
                    critical_path[user] > longest_user)
                longest_user = critical_path[user];
        if (UINT64_MAX - longest_user < operation_latency[index]) {
            status = scheduler_fail(
                diagnostic, diagnostic_capacity, ARCHON_SCHEDULER_LIMIT,
                "annotated scheduler critical path overflows");
            goto done;
        }
        critical_path[index] = operation_latency[index] + longest_user;
    }

    for (slot = 0; slot < count; ++slot) {
        size_t candidate = ARCHON_IR_NO_INDEX;
        for (index = 0; index < count; ++index) {
            size_t predecessor;
            int ready = !scheduled[index];
            if (!ready)
                continue;
            for (predecessor = 0; predecessor < count; ++predecessor)
                if (dependencies[index * count + predecessor] &&
                        !scheduled[predecessor]) {
                    ready = 0;
                    break;
                }
            if (!ready)
                continue;
            if (candidate == ARCHON_IR_NO_INDEX ||
                    critical_path[index] > critical_path[candidate] ||
                    (critical_path[index] == critical_path[candidate] &&
                     advisory_priority[index] >
                         advisory_priority[candidate]) ||
                    (critical_path[index] == critical_path[candidate] &&
                     advisory_priority[index] ==
                         advisory_priority[candidate] &&
                     index < candidate))
                candidate = index;
        }
        if (candidate == ARCHON_IR_NO_INDEX) {
            status = scheduler_fail(
                diagnostic, diagnostic_capacity, ARCHON_SCHEDULER_CYCLE,
                "dependency cycle in block '%s'", block->key);
            goto done;
        }
        if (!scheduler_emit_operation(
                output, &block->operations[candidate],
                block_operation_base + candidate, function_id, block_id,
                slot, &dependencies[candidate * count], count,
                block_operation_base, critical_path[candidate])) {
            status = scheduler_fail(diagnostic, diagnostic_capacity,
                                    ARCHON_SCHEDULER_IO_ERROR,
                                    "failed to write MIR.i");
            goto done;
        }
        scheduled[candidate] = 1u;
    }

done:
    free(advisory_priority);
    free(operation_latency);
    free(dependencies);
    free(scheduled);
    free(critical_path);
    return status;
}

void archon_scheduler_default_options(
        struct archon_scheduler_options *options) {
    if (!options)
        return;
    options->maximum_block_operations = 4096u;
@ORACLE_DEFAULT_OPTION@
}

@ORACLE_IMPL_LINKAGE@enum archon_scheduler_status @ORACLE_IMPL_NAME@(
        FILE *output, const struct archon_ir_program *program,
        const struct archon_scheduler_options *requested_options
        @ORACLE_IMPL_PARAMETER@,
        char *diagnostic, size_t diagnostic_capacity) {
    struct archon_scheduler_options defaults;
    const struct archon_scheduler_options *options = requested_options;
    char verification[256];
    size_t target_index, target_instruction_index;
    size_t function_index, operation_base = 0u;
    if (!output || !program)
        return scheduler_fail(diagnostic, diagnostic_capacity,
                              ARCHON_SCHEDULER_INVALID_ARGUMENT,
                              "null scheduler argument");
    if (archon_ir_verify(program, verification, sizeof verification) !=
            ARCHON_IR_OK)
        return scheduler_fail(diagnostic, diagnostic_capacity,
                              ARCHON_SCHEDULER_INVALID_IR,
                              "invalid ANF IR: %s", verification);
    if (!options) {
        archon_scheduler_default_options(&defaults);
        options = &defaults;
    }
    if (options->maximum_block_operations == 0u)
        return scheduler_fail(diagnostic, diagnostic_capacity,
                              ARCHON_SCHEDULER_INVALID_ARGUMENT,
                              "invalid scheduler options");
    if (fputs("MIR_SCHEMA(2)\nMIR_PROGRAM_BEGIN(", output) < 0 ||
            !scheduler_put_string(output, program->name) ||
            fputs(")\n", output) < 0)
        return scheduler_fail(diagnostic, diagnostic_capacity,
                              ARCHON_SCHEDULER_IO_ERROR,
                              "failed to write MIR.i");
    for (target_index = 0; target_index < scheduler_target_count;
         ++target_index) {
        if (fprintf(output, "MIR_TARGET(%zu, ", target_index) < 0 ||
                !scheduler_put_string(output,
                                      scheduler_targets[target_index].name) ||
                fprintf(output, ", %u)\n",
                        scheduler_targets[target_index].address_width) < 0)
            return scheduler_fail(diagnostic, diagnostic_capacity,
                                  ARCHON_SCHEDULER_IO_ERROR,
                                  "failed to write MIR.i");
    }
    for (target_instruction_index = 0;
         target_instruction_index < scheduler_target_instruction_count;
         ++target_instruction_index) {
        const struct archon_scheduler_target_instruction *instruction =
            &scheduler_target_instructions[target_instruction_index];
        if (fprintf(output, "MIR_TARGET_INSTRUCTION(%u, %u, ",
                    instruction->target, instruction->ordinal) < 0 ||
                !scheduler_put_string(output, instruction->instruction) ||
                fprintf(output,
                    ", %u, %u, 0, 0, 0, 0, \"\", 0)\n",
                    instruction->encoding_bytes,
                    instruction->encoding_bytes_known) < 0)
            return scheduler_fail(diagnostic, diagnostic_capacity,
                                  ARCHON_SCHEDULER_IO_ERROR,
                                  "failed to write MIR.i");
    }
    for (function_index = 0;
         function_index < program->function_count; ++function_index) {
        const struct archon_ir_function *function =
            &program->functions[function_index];
        size_t value_index, block_index;
        if (fprintf(output, "MIR_FUNCTION_BEGIN(%zu, ", function_index) < 0 ||
                !scheduler_put_string(output, function->key) ||
                fputs(", ", output) < 0 ||
                !scheduler_put_string(output, function->name) ||
                fprintf(output, ", %s, %u)\n",
                        scheduler_type_token(function->result_type.kind),
                        function->result_type.width) < 0)
            return scheduler_fail(diagnostic, diagnostic_capacity,
                                  ARCHON_SCHEDULER_IO_ERROR,
                                  "failed to write MIR.i");
        for (value_index = 0; value_index < function->value_count;
             ++value_index) {
            const struct archon_ir_value *value =
                &function->values[value_index];
            if (fprintf(output, "MIR_VALUE(%zu, %zu, ",
                        function_index, value_index) < 0 ||
                    !scheduler_put_string(output, value->key) ||
                    fputs(", ", output) < 0 ||
                    !scheduler_put_string(output, value->name) ||
                    fprintf(output, ", %s, %u, %d)\n",
                            scheduler_type_token(value->type.kind),
                            value->type.width,
                            value->is_parameter != 0) < 0)
                return scheduler_fail(diagnostic, diagnostic_capacity,
                                      ARCHON_SCHEDULER_IO_ERROR,
                                      "failed to write MIR.i");
        }
        for (block_index = 0; block_index < function->block_count;
             ++block_index) {
            const struct archon_ir_block *block =
                &function->blocks[block_index];
            enum archon_scheduler_status status;
            if (fprintf(output, "MIR_BLOCK_BEGIN(%zu, %zu, ",
                        function_index, block_index) < 0 ||
                    !scheduler_put_string(output, block->key) ||
                    fputs(", ", output) < 0 ||
                    !scheduler_put_string(output, block->name) ||
                    fputs(")\n", output) < 0)
                return scheduler_fail(diagnostic, diagnostic_capacity,
                                      ARCHON_SCHEDULER_IO_ERROR,
                                      "failed to write MIR.i");
            status = scheduler_schedule_block(
                output, function, function_index, block, block_index,
                operation_base @ORACLE_BLOCK_ARGUMENT@, options,
                diagnostic, diagnostic_capacity);
            if (status != ARCHON_SCHEDULER_OK)
                return status;
            if (fprintf(output, "MIR_BLOCK_END(%zu, %zu)\n",
                        function_index, block_index) < 0)
                return scheduler_fail(diagnostic, diagnostic_capacity,
                                      ARCHON_SCHEDULER_IO_ERROR,
                                      "failed to write MIR.i");
            operation_base += block->operation_count;
        }
        if (fprintf(output, "MIR_FUNCTION_END(%zu)\n",
                    function_index) < 0)
            return scheduler_fail(diagnostic, diagnostic_capacity,
                                  ARCHON_SCHEDULER_IO_ERROR,
                                  "failed to write MIR.i");
    }
    if (fputs("MIR_PROGRAM_END()\n", output) < 0 || fflush(output) != 0)
        return scheduler_fail(diagnostic, diagnostic_capacity,
                              ARCHON_SCHEDULER_IO_ERROR,
                              "failed to write MIR.i");
    if (diagnostic && diagnostic_capacity)
        diagnostic[0] = '\0';
    return ARCHON_SCHEDULER_OK;
}

@ORACLE_WRAPPER@

enum archon_scheduler_status archon_scheduler_emit_embedded(
        FILE *output, const struct archon_scheduler_options *options,
        char *diagnostic, size_t diagnostic_capacity) {
    struct archon_ir_program program;
    char ir_diagnostic[256];
    enum archon_scheduler_status status;
    if (archon_ir_load_embedded(&program, ir_diagnostic,
                                sizeof ir_diagnostic) != ARCHON_IR_OK)
        return scheduler_fail(diagnostic, diagnostic_capacity,
                              ARCHON_SCHEDULER_INVALID_IR,
                              "cannot load Program.i: %s", ir_diagnostic);
    status = archon_scheduler_emit_mir(
        output, &program, options, diagnostic, diagnostic_capacity);
    archon_ir_program_dispose(&program);
    return status;
}

const char *archon_scheduler_status_name(
        enum archon_scheduler_status status) {
    static const char *const names[] = {
        "ok", "invalid argument", "invalid IR", "out of memory",
        "limit exceeded", "I/O error", "dependency cycle"
    };
    size_t index = (size_t)status;
    return index < sizeof names / sizeof names[0] ? names[index]
                                                  : "unknown status";
}
'''


__all__ = ["NAME", "ALIASES", "emit"]
