"""Generate a standalone C99 codec and transactional architectural emulator."""

import re

from .c99 import generate_codec
from .c99_semantics import lower_instruction
from .database import _validate
from .diagnostics import ArchonError


_EXTENDED_EFFECTS = (
    "state_reads", "state_writes", "stack_reads", "stack_writes",
    "memory_reads", "memory_writes",
)


def _state(db):
    return db.get("state", {"scalars": [], "stacks": [], "memories": []})


def _register_model(db):
    registers = db["registers"]
    if registers is None:
        return 0, 0, None
    return registers["count"], registers["width"], registers["zero"]


def _validated_codec(db, prefix):
    # The database schema checks typed SemIR; the codec also checks the
    # relationships between encoding layouts, operands, and instructions.
    _validate(db)
    count, width, _ = _register_model(db)
    if count and width > 64:
        raise ArchonError("the C99 emulator requires register widths of up to 64 bits")

    state = _state(db)
    for row in state["scalars"]:
        if row["type"]["width"] > 64:
            raise ArchonError(
                f"the C99 emulator requires scalar state {row['name']!r} to be at most 64 bits"
            )
    for row in state["stacks"]:
        if row["element_type"]["width"] > 64:
            raise ArchonError(
                f"the C99 emulator requires stack {row['name']!r} elements to be at most 64 bits"
            )

    for inst in db["instructions"]:
        if inst["semantics"] is None:
            continue
        statements = list(inst["semantics"]["statements"])
        while statements:
            stmt = statements.pop()
            if stmt["op"] in ("sequence", "parallel"):
                statements.extend(stmt["statements"])
                continue
            expressions = [
                value for key in ("value", "address")
                if isinstance(value := stmt.get(key), dict)
            ]
            while expressions:
                expr = expressions.pop()
                if expr["type"]["width"] > 64:
                    raise ArchonError(
                        f"the C99 emulator requires semantic widths of up to 64 bits "
                        f"in {inst['name']!r}"
                    )
                for key in ("left", "right", "value", "address"):
                    if isinstance(child := expr.get(key), dict):
                        expressions.append(child)
    return generate_codec(db, prefix)


def _render(template, prefix, **replacements):
    replacements.update(PREFIX=prefix, UPPER=prefix.upper())
    return re.sub(r"@([A-Z]+)@", lambda match: str(replacements[match[1]]), template)


def _state_types(db, prefix):
    count, width, _ = _register_model(db)
    state = _state(db)
    upper = prefix.upper()
    limits = [
        f"    {upper}_EMU_REGISTER_COUNT = {count},",
        f"    {upper}_EMU_REGISTER_WIDTH = {width},",
    ]
    members = ["    uint64_t *registers;"]
    for row in state["scalars"]:
        name = row["name"]
        limits.append(
            f"    {upper}_EMU_SCALAR_{name}_WIDTH = {row['type']['width']},"
        )
        members.append(f"    uint64_t scalar_{name};")
    for row in state["stacks"]:
        name = row["name"]
        limits.extend((
            f"    {upper}_EMU_STACK_{name}_WIDTH = {row['element_type']['width']},",
            f"    {upper}_EMU_STACK_{name}_CAPACITY = {row['capacity']},",
        ))
        members.append(f"    struct {prefix}_emu_stack stack_{name};")
    for row in state["memories"]:
        name = row["name"]
        limits.append(f"    {upper}_EMU_MEMORY_{name}_SIZE = {row['size']},")
        members.append(f"    uint8_t *memory_{name};")
    return f"""enum {prefix}_emu_limit {{
{chr(10).join(limits)}
}};

struct {prefix}_emu_stack {{
    uint64_t *values;
    size_t size;
}};

struct {prefix}_emu_state {{
{chr(10).join(members)}
}};
"""


def _transaction_fragments(db, prefix):
    count, _, _ = _register_model(db)
    state = _state(db)
    fields = ["    int prepared;", "    uint64_t *registers;"]
    release = ["    free(transaction->registers);"]
    allocate = []
    snapshot = [
        "    transaction->scratch = *state;",
        "    transaction->scratch.registers = transaction->registers;",
    ]
    commit = ["    (void)state;", "    (void)transaction;"]
    allocations = 0

    if count:
        allocations += 1
        allocate.extend((
            f"    transaction->registers = malloc({count}u * sizeof *transaction->registers);",
            "    if (!transaction->registers)",
            "        goto allocation_failure;",
        ))
        snapshot.append(
            f"    memcpy(transaction->registers, state->registers, "
            f"{count}u * sizeof *transaction->registers);"
        )
        commit.append(
            f"    memcpy(state->registers, transaction->scratch.registers, "
            f"{count}u * sizeof *state->registers);"
        )

    for row in state["scalars"]:
        name = row["name"]
        commit.append(
            f"    state->scalar_{name} = transaction->scratch.scalar_{name};"
        )

    for row in state["stacks"]:
        name, capacity = row["name"], row["capacity"]
        field = f"stack_{name}"
        fields.append(f"    uint64_t *{field};")
        release.append(f"    free(transaction->{field});")
        allocations += 1
        allocate.extend((
            f"    transaction->{field} = malloc({capacity}u * sizeof *transaction->{field});",
            f"    if (!transaction->{field})",
            "        goto allocation_failure;",
        ))
        snapshot.extend((
            f"    transaction->scratch.{field}.values = transaction->{field};",
            f"    memcpy(transaction->{field}, state->{field}.values, "
            f"{capacity}u * sizeof *transaction->{field});",
        ))
        commit.extend((
            f"    memcpy(state->{field}.values, transaction->scratch.{field}.values, "
            f"{capacity}u * sizeof *state->{field}.values);",
            f"    state->{field}.size = transaction->scratch.{field}.size;",
        ))

    for row in state["memories"]:
        name, size = row["name"], row["size"]
        field = f"memory_{name}"
        fields.append(f"    uint8_t *{field};")
        release.append(f"    free(transaction->{field});")
        allocations += 1
        allocate.extend((
            f"    transaction->{field} = malloc({size}u);",
            f"    if (!transaction->{field})",
            "        goto allocation_failure;",
        ))
        snapshot.extend((
            f"    transaction->scratch.{field} = transaction->{field};",
            f"    memcpy(transaction->{field}, state->{field}, {size}u);",
        ))
        commit.append(
            f"    memcpy(state->{field}, transaction->scratch.{field}, {size}u);"
        )

    fields.append(f"    struct {prefix}_emu_state scratch;")
    failure = ""
    if allocations:
        failure = (
            "allocation_failure:\n"
            f"    {prefix}_emu_transaction_release(transaction);\n"
            f"    return {prefix.upper()}_EMU_ALLOCATION_FAILURE;"
        )
    return {
        "TRANSACTIONFIELDS": "\n".join(fields),
        "TRANSACTIONRELEASE": "\n".join(release),
        "TRANSACTIONALLOCATE": "\n".join(allocate),
        "TRANSACTIONFAILURE": failure,
        "TRANSACTIONSNAPSHOT": "\n".join(snapshot),
        "TRANSACTIONCOMMIT": "\n".join(commit),
    }


def _valid_state_body(db, prefix):
    count, width, _ = _register_model(db)
    state = _state(db)
    lines = ["    if (!state)", "        return 0;"]
    if count:
        lines.extend((
            "    if (!state->registers)",
            "        return 0;",
            f"    for (size_t i = 0; i < {count}u; ++i)",
            f"        if (state->registers[i] > {prefix}_low_mask({width}u))",
            "            return 0;",
        ))
    for row in state["scalars"]:
        lines.extend((
            f"    if (state->scalar_{row['name']} > "
            f"{prefix}_low_mask({row['type']['width']}u))",
            "        return 0;",
        ))
    for row in state["stacks"]:
        name, width, capacity = (
            row["name"], row["element_type"]["width"], row["capacity"],
        )
        lines.extend((
            f"    if (!state->stack_{name}.values || "
            f"state->stack_{name}.size > {capacity}u)",
            "        return 0;",
            f"    for (size_t i = 0; i < state->stack_{name}.size; ++i)",
            f"        if (state->stack_{name}.values[i] > "
            f"{prefix}_low_mask({width}u))",
            "            return 0;",
        ))
    for row in state["memories"]:
        lines.extend((
            f"    if (!state->memory_{row['name']})",
            "        return 0;",
        ))
    lines.append("    return 1;")
    return "\n".join(lines)


def _needs_extended_state(inst):
    if inst["semantics"] is None:
        return False
    effects = inst["semantics"]["effects"]
    return any(effects.get(key) for key in _EXTENDED_EFFECTS)


def generate_emulator(db: dict, prefix: str = "archon") -> str:
    """Return a standalone C99 translation unit exporting the codec and emulator."""
    codec = _validated_codec(db, prefix)
    count, width, zero = _register_model(db)
    state = _state(db)
    instructions = sorted(db["instructions"], key=lambda inst: inst["name"])
    functions, rows, needs_state = [], [], []
    for index, inst in enumerate(instructions):
        if inst["semantics"] is None:
            rows.append("    NULL,")
        else:
            functions.append(lower_instruction(
                inst, prefix, index, inst["semantics"]["word_width"], zero,
                state=state, endian=db["endian"],
            ))
            rows.append(f"    {prefix}_emu_sem_{index},")
        needs_state.append(f"    {int(_needs_extended_state(inst))},")

    normalize = f"    (*scratch)[{zero}] = 0;\n" if zero is not None else ""
    state_normalize = (
        f"    transaction->scratch.registers[{zero}] = 0;\n"
        if zero is not None else ""
    )
    replacements = {
        "REGISTERS": count,
        "WIDTH": width,
        "BYTES": instructions[0]["width"] // 8 if instructions else 0,
        "OPERANDS": max(1, max((len(inst["operands"]) for inst in instructions), default=0)),
        "FUNCTIONS": "\n".join(functions),
        "ROWS": "\n".join(rows or ["    NULL,"]),
        "NEEDSSTATE": "\n".join(needs_state or ["    0,"]),
        "NORMALIZE": normalize,
        "STATENORMALIZE": state_normalize,
        "VALIDSTATE": _valid_state_body(db, prefix),
    }
    replacements.update(_transaction_fragments(db, prefix))
    api = _state_types(db, prefix) + "\n" + _EMU_API
    return codec + _render(
        "\n#include <stdlib.h>\n\n" + api + "\n" + _EMU,
        prefix,
        **replacements,
    )


def generate_emulator_header(db: dict, prefix: str = "archon") -> str:
    """Return the public header for a translation unit from generate_emulator."""
    _validated_codec(db, prefix)
    api = _state_types(db, prefix) + "\n" + _EMU_API
    return _render(
        """/* Generated by Archon. Edit the architecture source, not this file. */
#ifndef @UPPER@_EMU_H_INCLUDED
#define @UPPER@_EMU_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

"""
        + _CODEC_API + "\n" + api + "\n#endif\n",
        prefix,
    )


_CODEC_API = r"""enum @PREFIX@_status {
    @UPPER@_OK,
    @UPPER@_INVALID_ARGUMENT,
    @UPPER@_UNKNOWN_INSTRUCTION,
    @UPPER@_OPERAND_RANGE,
    @UPPER@_BUFFER_TOO_SMALL
};

enum @PREFIX@_status @PREFIX@_encode(const char *name, const uint64_t *operands,
        size_t operand_count, uint8_t *out, size_t out_capacity, size_t *out_size);
enum @PREFIX@_status @PREFIX@_decode(const uint8_t *data, size_t data_len,
        const char **name, uint64_t *operands, size_t operand_capacity, size_t *operand_count);
"""


_EMU_API = r"""enum @PREFIX@_emu_status {
    @UPPER@_EMU_OK,
    @UPPER@_EMU_INVALID_ARGUMENT,
    @UPPER@_EMU_UNKNOWN_INSTRUCTION,
    @UPPER@_EMU_OPERAND_RANGE,
    @UPPER@_EMU_NO_SEMANTICS,
    @UPPER@_EMU_CONFLICTING_WRITES,
    @UPPER@_EMU_TRUNCATED_INSTRUCTION,
    @UPPER@_EMU_ALLOCATION_FAILURE,
    @UPPER@_EMU_STATE_REQUIRED,
    @UPPER@_EMU_STACK_UNDERFLOW,
    @UPPER@_EMU_STACK_OVERFLOW,
    @UPPER@_EMU_MEMORY_OUT_OF_BOUNDS
};

size_t @PREFIX@_emu_register_count(void);
unsigned @PREFIX@_emu_register_width(void);

/*
 * Legacy register-only API. Registers contain exactly the architectural
 * number of unsigned, word-width values. An instruction that accesses scalar,
 * stack, or memory state returns STATE_REQUIRED. An error changes no register.
 */
enum @PREFIX@_emu_status @PREFIX@_emu_execute(const char *name, const uint64_t *operands,
        size_t operand_count, uint64_t *registers, size_t register_count);
enum @PREFIX@_emu_status @PREFIX@_emu_step(const uint8_t *data, size_t size,
        uint64_t *registers, size_t register_count);
enum @PREFIX@_emu_status @PREFIX@_emu_run(const uint8_t *data, size_t size,
        uint64_t *registers, size_t register_count, size_t *bytes_executed);

/*
 * Complete-state API. The state owns all pointed-to buffers. Every pointer
 * required by the generated struct must address the capacity/size declared by
 * the corresponding enum constant. A failed instruction changes no state.
 */
enum @PREFIX@_emu_status @PREFIX@_emu_execute_state(const char *name,
        const uint64_t *operands, size_t operand_count, struct @PREFIX@_emu_state *state);
enum @PREFIX@_emu_status @PREFIX@_emu_step_state(const uint8_t *data, size_t size,
        struct @PREFIX@_emu_state *state);
enum @PREFIX@_emu_status @PREFIX@_emu_run_state(const uint8_t *data, size_t size,
        struct @PREFIX@_emu_state *state, size_t *bytes_executed);
"""


_EMU = r"""/* The codec tables and dispatch tables share name-sorted indices. */
@FUNCTIONS@
typedef enum @PREFIX@_emu_status (*@PREFIX@_emu_fn)(
        const uint64_t *, uint64_t *, struct @PREFIX@_emu_state *);
static const @PREFIX@_emu_fn @PREFIX@_emu_dispatch[] = {
@ROWS@
};
static const unsigned char @PREFIX@_emu_needs_state[] = {
@NEEDSSTATE@
};
static const size_t @PREFIX@_emu_instruction_bytes = @BYTES@;

#if SIZE_MAX / 8 < @REGISTERS@
#error "Archon emulator register vector is not representable in size_t"
#endif

size_t @PREFIX@_emu_register_count(void) {
    return @REGISTERS@;
}

unsigned @PREFIX@_emu_register_width(void) {
    return @WIDTH@;
}

static int @PREFIX@_emu_valid_registers(const uint64_t *registers, size_t register_count) {
    size_t i;
    if (register_count != @PREFIX@_emu_register_count() ||
            (register_count != 0 && !registers))
        return 0;
    for (i = 0; i < register_count; ++i)
        if (registers[i] > @PREFIX@_low_mask(@WIDTH@))
            return 0;
    return 1;
}

static int @PREFIX@_emu_valid_state(const struct @PREFIX@_emu_state *state) {
@VALIDSTATE@
}

static size_t @PREFIX@_emu_find(const char *name) {
    size_t i;
    for (i = 0; i < @PREFIX@_count; ++i)
        if (strcmp(name, @PREFIX@_instructions[i].name) == 0)
            break;
    return i;
}

static enum @PREFIX@_emu_status @PREFIX@_emu_validate_operands(
        const struct @PREFIX@_instruction *inst, const uint64_t *operands,
        size_t operand_count) {
    size_t i;
    if (operand_count != inst->operand_count || (operand_count && !operands))
        return @UPPER@_EMU_INVALID_ARGUMENT;
    for (i = 0; i < operand_count; ++i) {
        const struct @PREFIX@_field *field = &@PREFIX@_fields[inst->first_field + i];
        uint64_t value = operands[i], low = value & @PREFIX@_low_mask(field->width);
        if ((field->register_limit && value >= field->register_limit) ||
                value != (field->is_signed ? @PREFIX@_sign_extend(low, field->width) : low))
            return @UPPER@_EMU_OPERAND_RANGE;
    }
    return @UPPER@_EMU_OK;
}

/* Allocate the legacy register transaction buffer only once per call. */
static enum @PREFIX@_emu_status @PREFIX@_emu_apply(size_t index, const uint64_t *operands,
        uint64_t *registers, uint64_t **scratch) {
    const size_t bytes = @PREFIX@_emu_register_count() * sizeof *registers;
    @PREFIX@_emu_fn semantics = @PREFIX@_emu_dispatch[index];
    enum @PREFIX@_emu_status status;
    if (!semantics)
        return @UPPER@_EMU_NO_SEMANTICS;
    if (@PREFIX@_emu_needs_state[index])
        return @UPPER@_EMU_STATE_REQUIRED;
    if (bytes && !*scratch) {
        *scratch = malloc(bytes);
        if (!*scratch)
            return @UPPER@_EMU_ALLOCATION_FAILURE;
    }
    if (bytes)
        memcpy(*scratch, registers, bytes);
@NORMALIZE@    status = semantics(operands, *scratch, NULL);
    if (status == @UPPER@_EMU_OK && bytes)
        memcpy(registers, *scratch, bytes);
    return status;
}

struct @PREFIX@_emu_transaction {
@TRANSACTIONFIELDS@
};

static void @PREFIX@_emu_transaction_release(
        struct @PREFIX@_emu_transaction *transaction) {
@TRANSACTIONRELEASE@
    memset(transaction, 0, sizeof *transaction);
}

static enum @PREFIX@_emu_status @PREFIX@_emu_transaction_prepare(
        struct @PREFIX@_emu_transaction *transaction) {
    if (transaction->prepared)
        return @UPPER@_EMU_OK;
@TRANSACTIONALLOCATE@
    transaction->prepared = 1;
    return @UPPER@_EMU_OK;
@TRANSACTIONFAILURE@
}

static void @PREFIX@_emu_transaction_snapshot(
        struct @PREFIX@_emu_transaction *transaction,
        const struct @PREFIX@_emu_state *state) {
@TRANSACTIONSNAPSHOT@
}

static void @PREFIX@_emu_transaction_commit(
        struct @PREFIX@_emu_state *state,
        const struct @PREFIX@_emu_transaction *transaction) {
@TRANSACTIONCOMMIT@
}

static enum @PREFIX@_emu_status @PREFIX@_emu_apply_state(
        size_t index, const uint64_t *operands, struct @PREFIX@_emu_state *state,
        struct @PREFIX@_emu_transaction *transaction) {
    @PREFIX@_emu_fn semantics = @PREFIX@_emu_dispatch[index];
    enum @PREFIX@_emu_status status;
    if (!semantics)
        return @UPPER@_EMU_NO_SEMANTICS;
    status = @PREFIX@_emu_transaction_prepare(transaction);
    if (status != @UPPER@_EMU_OK)
        return status;
    @PREFIX@_emu_transaction_snapshot(transaction, state);
@STATENORMALIZE@    status = semantics(
        operands, transaction->scratch.registers, &transaction->scratch);
    if (status == @UPPER@_EMU_OK)
        @PREFIX@_emu_transaction_commit(state, transaction);
    return status;
}

enum @PREFIX@_emu_status @PREFIX@_emu_execute(const char *name, const uint64_t *operands,
        size_t operand_count, uint64_t *registers, size_t register_count) {
    const struct @PREFIX@_instruction *inst;
    uint64_t *scratch = NULL;
    enum @PREFIX@_emu_status status;
    size_t index;
    if (!name || !@PREFIX@_emu_valid_registers(registers, register_count))
        return @UPPER@_EMU_INVALID_ARGUMENT;
    index = @PREFIX@_emu_find(name);
    if (index == @PREFIX@_count)
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    inst = &@PREFIX@_instructions[index];
    status = @PREFIX@_emu_validate_operands(inst, operands, operand_count);
    if (status == @UPPER@_EMU_OK)
        status = @PREFIX@_emu_apply(index, operands, registers, &scratch);
    free(scratch);
    return status;
}

enum @PREFIX@_emu_status @PREFIX@_emu_execute_state(const char *name,
        const uint64_t *operands, size_t operand_count, struct @PREFIX@_emu_state *state) {
    const struct @PREFIX@_instruction *inst;
    struct @PREFIX@_emu_transaction transaction = {0};
    enum @PREFIX@_emu_status status;
    size_t index;
    if (!name || !@PREFIX@_emu_valid_state(state))
        return @UPPER@_EMU_INVALID_ARGUMENT;
    index = @PREFIX@_emu_find(name);
    if (index == @PREFIX@_count)
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    inst = &@PREFIX@_instructions[index];
    status = @PREFIX@_emu_validate_operands(inst, operands, operand_count);
    if (status == @UPPER@_EMU_OK)
        status = @PREFIX@_emu_apply_state(index, operands, state, &transaction);
    @PREFIX@_emu_transaction_release(&transaction);
    return status;
}

static enum @PREFIX@_emu_status @PREFIX@_emu_chunk(const uint8_t *data,
        uint64_t *registers, uint64_t **scratch) {
    uint64_t operands[@OPERANDS@];
    const char *name;
    size_t count, index;
    switch (@PREFIX@_decode(data, @PREFIX@_emu_instruction_bytes, &name, operands,
                sizeof operands / sizeof *operands, &count)) {
    case @UPPER@_OK:
        index = @PREFIX@_emu_find(name);
        if (index < @PREFIX@_count)
            return @PREFIX@_emu_apply(index, operands, registers, scratch);
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    case @UPPER@_UNKNOWN_INSTRUCTION:
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    case @UPPER@_OPERAND_RANGE:
        return @UPPER@_EMU_OPERAND_RANGE;
    default:
        return @UPPER@_EMU_INVALID_ARGUMENT;
    }
}

static enum @PREFIX@_emu_status @PREFIX@_emu_chunk_state(const uint8_t *data,
        struct @PREFIX@_emu_state *state,
        struct @PREFIX@_emu_transaction *transaction) {
    uint64_t operands[@OPERANDS@];
    const char *name;
    size_t count, index;
    switch (@PREFIX@_decode(data, @PREFIX@_emu_instruction_bytes, &name, operands,
                sizeof operands / sizeof *operands, &count)) {
    case @UPPER@_OK:
        index = @PREFIX@_emu_find(name);
        if (index < @PREFIX@_count)
            return @PREFIX@_emu_apply_state(index, operands, state, transaction);
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    case @UPPER@_UNKNOWN_INSTRUCTION:
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    case @UPPER@_OPERAND_RANGE:
        return @UPPER@_EMU_OPERAND_RANGE;
    default:
        return @UPPER@_EMU_INVALID_ARGUMENT;
    }
}

enum @PREFIX@_emu_status @PREFIX@_emu_step(const uint8_t *data, size_t size,
        uint64_t *registers, size_t register_count) {
    uint64_t *scratch = NULL;
    enum @PREFIX@_emu_status status;
    if ((size && !data) || !@PREFIX@_emu_valid_registers(registers, register_count))
        return @UPPER@_EMU_INVALID_ARGUMENT;
    if (!@PREFIX@_emu_instruction_bytes)
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    if (size < @PREFIX@_emu_instruction_bytes)
        return @UPPER@_EMU_TRUNCATED_INSTRUCTION;
    if (size > @PREFIX@_emu_instruction_bytes)
        return @UPPER@_EMU_INVALID_ARGUMENT;
    status = @PREFIX@_emu_chunk(data, registers, &scratch);
    free(scratch);
    return status;
}

enum @PREFIX@_emu_status @PREFIX@_emu_step_state(const uint8_t *data, size_t size,
        struct @PREFIX@_emu_state *state) {
    struct @PREFIX@_emu_transaction transaction = {0};
    enum @PREFIX@_emu_status status;
    if ((size && !data) || !@PREFIX@_emu_valid_state(state))
        return @UPPER@_EMU_INVALID_ARGUMENT;
    if (!@PREFIX@_emu_instruction_bytes)
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    if (size < @PREFIX@_emu_instruction_bytes)
        return @UPPER@_EMU_TRUNCATED_INSTRUCTION;
    if (size > @PREFIX@_emu_instruction_bytes)
        return @UPPER@_EMU_INVALID_ARGUMENT;
    status = @PREFIX@_emu_chunk_state(data, state, &transaction);
    @PREFIX@_emu_transaction_release(&transaction);
    return status;
}

enum @PREFIX@_emu_status @PREFIX@_emu_run(const uint8_t *data, size_t size,
        uint64_t *registers, size_t register_count, size_t *bytes_executed) {
    uint64_t *scratch = NULL;
    enum @PREFIX@_emu_status status = @UPPER@_EMU_OK;
    const size_t chunk_size = @PREFIX@_emu_instruction_bytes;
    size_t offset = 0;
    if (!bytes_executed)
        return @UPPER@_EMU_INVALID_ARGUMENT;
    *bytes_executed = 0;
    if ((size && !data) || !@PREFIX@_emu_valid_registers(registers, register_count))
        return @UPPER@_EMU_INVALID_ARGUMENT;
    if (size && !chunk_size)
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    while (offset < size) {
        if (size - offset < chunk_size) {
            status = @UPPER@_EMU_TRUNCATED_INSTRUCTION;
            break;
        }
        status = @PREFIX@_emu_chunk(data + offset, registers, &scratch);
        if (status != @UPPER@_EMU_OK)
            break;
        offset += chunk_size;
    }
    free(scratch);
    *bytes_executed = offset;
    return status;
}

enum @PREFIX@_emu_status @PREFIX@_emu_run_state(const uint8_t *data, size_t size,
        struct @PREFIX@_emu_state *state, size_t *bytes_executed) {
    struct @PREFIX@_emu_transaction transaction = {0};
    enum @PREFIX@_emu_status status = @UPPER@_EMU_OK;
    const size_t chunk_size = @PREFIX@_emu_instruction_bytes;
    size_t offset = 0;
    if (!bytes_executed)
        return @UPPER@_EMU_INVALID_ARGUMENT;
    *bytes_executed = 0;
    if ((size && !data) || !@PREFIX@_emu_valid_state(state))
        return @UPPER@_EMU_INVALID_ARGUMENT;
    if (size && !chunk_size)
        return @UPPER@_EMU_UNKNOWN_INSTRUCTION;
    while (offset < size) {
        if (size - offset < chunk_size) {
            status = @UPPER@_EMU_TRUNCATED_INSTRUCTION;
            break;
        }
        status = @PREFIX@_emu_chunk_state(data + offset, state, &transaction);
        if (status != @UPPER@_EMU_OK)
            break;
        offset += chunk_size;
    }
    @PREFIX@_emu_transaction_release(&transaction);
    *bytes_executed = offset;
    return status;
}
"""
