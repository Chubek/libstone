#ifndef SANDSTONE_DSYMSPEC_H
#define SANDSTONE_DSYMSPEC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Each generated .i has internal linkage and can be included with other
 * targets. Declaration and operand order is significant; map key order is not.
 */
typedef enum dsymspec_decl_kind {
    DSYMSPEC_DECL_TARGET, DSYMSPEC_DECL_FEATURE, DSYMSPEC_DECL_REGISTER,
    DSYMSPEC_DECL_MNEMONIC, DSYMSPEC_DECL_ENCODER, DSYMSPEC_DECL_FORM,
    DSYMSPEC_DECL_SYNTAX, DSYMSPEC_DECL_SECTION, DSYMSPEC_DECL_LINKAGE,
    DSYMSPEC_DECL_RELOC, DSYMSPEC_DECL_DEBUG
} dsymspec_decl_kind;

typedef enum dsymspec_value_kind {
    DSYMSPEC_VALUE_BOOL, DSYMSPEC_VALUE_INTEGER, DSYMSPEC_VALUE_REAL,
    DSYMSPEC_VALUE_STRING, DSYMSPEC_VALUE_LIST, DSYMSPEC_VALUE_RECORD,
    DSYMSPEC_VALUE_TABLE
} dsymspec_value_kind;

struct dsymspec_field;
typedef struct dsymspec_value {
    dsymspec_value_kind kind;
    int64_t integer;                  /* BOOL or INTEGER */
    double real;                      /* REAL */
    const char *string;               /* STRING, owned by the generated file */
    size_t count;                     /* string bytes or number of fields */
    const struct dsymspec_field *fields; /* LIST, RECORD, or mixed/sparse TABLE */
} dsymspec_value;

typedef struct dsymspec_field {
    const char *name;                   /* NULL for an integer key */
    int64_t index;                     /* raw Lua table key, including for lists */
    dsymspec_value value;
} dsymspec_field;

/* Return NULL for an absent key; there is no coercion or defaulting. */
static inline const dsymspec_value *dsymspec_get(const dsymspec_value *value,
                                                const char *name) {
    size_t i;
    if (!value || !name || !value->fields) return NULL;
    for (i = 0; i < value->count; ++i)
        if (value->fields[i].name && !strcmp(value->fields[i].name, name))
            return &value->fields[i].value;
    return NULL;
}

/* List subscripts in this convenience API are zero-based. */
static inline const dsymspec_value *dsymspec_at(const dsymspec_value *value,
                                               size_t index) {
    return value && value->kind == DSYMSPEC_VALUE_LIST && index < value->count
        ? &value->fields[index].value : NULL;
}

/* Look up an arbitrary numeric Lua key (unlike dsymspec_at's zero-based subscript). */
static inline const dsymspec_value *dsymspec_get_index(const dsymspec_value *value,
                                                      int64_t key) {
    size_t i;
    if (!value || !value->fields) return NULL;
    for (i = 0; i < value->count; ++i)
        if (!value->fields[i].name && value->fields[i].index == key)
            return &value->fields[i].value;
    return NULL;
}

typedef enum dsymspec_expr_kind {
    DSYMSPEC_EXPR_INT, DSYMSPEC_EXPR_STRING, DSYMSPEC_EXPR_BINDING,
    DSYMSPEC_EXPR_NEG, DSYMSPEC_EXPR_NOT, DSYMSPEC_EXPR_LOW8, DSYMSPEC_EXPR_SELECT,
    DSYMSPEC_EXPR_ADD, DSYMSPEC_EXPR_SUB, DSYMSPEC_EXPR_MUL, DSYMSPEC_EXPR_DIV,
    DSYMSPEC_EXPR_MOD, DSYMSPEC_EXPR_EQ, DSYMSPEC_EXPR_NE, DSYMSPEC_EXPR_LT,
    DSYMSPEC_EXPR_LE, DSYMSPEC_EXPR_GT, DSYMSPEC_EXPR_GE, DSYMSPEC_EXPR_AND,
    DSYMSPEC_EXPR_OR
} dsymspec_expr_kind;

typedef struct dsymspec_expr {
    dsymspec_expr_kind kind;
    uint32_t args[3];                 /* expression indices */
    int64_t integer;
    const char *text;
} dsymspec_expr;

typedef enum dsymspec_op_kind {
    DSYMSPEC_OP_EMIT, DSYMSPEC_OP_EMIT_SIZED, DSYMSPEC_OP_MODRM, DSYMSPEC_OP_SIB,
    DSYMSPEC_OP_REQUIRE, DSYMSPEC_OP_IF_FALSE, DSYMSPEC_OP_JUMP,
    DSYMSPEC_OP_RELOC_NAME, DSYMSPEC_OP_RELOC_ID, DSYMSPEC_OP_RELOC_DYNAMIC,
    DSYMSPEC_OP_FAIL
} dsymspec_op_kind;

/* Unused expression arguments are UINT32_MAX; jumps are zero-based op indices.
 * EMIT's detail is the byte width: positive for LE, negative for BE.
 * RELOC_NAME uses text; RELOC_ID uses detail; RELOC_DYNAMIC evaluates args[1].
 * For relocation ops args[0] is the symbol and args[2] is an optional addend.
 */
typedef struct dsymspec_op {
    dsymspec_op_kind kind;
    uint32_t args[3];
    uint32_t jump;
    int64_t detail;
    const char *text;
} dsymspec_op;

typedef struct dsymspec_encoder {
    size_t expr_count;
    const dsymspec_expr *exprs;
    size_t op_count;
    const dsymspec_op *ops;
    size_t binding_count;
    const char *const *binding_names;  /* Potential reads; branches can omit some. */
    size_t max_bytes;
    size_t max_relocs;
    const char *unsupported_reason;   /* Non-NULL: no executable recipe. */
} dsymspec_encoder;

typedef struct dsymspec_decl {
    dsymspec_decl_kind kind;
    const char *name;                  /* arch, mnemonic, message or record name */
    const dsymspec_value *record;
    const dsymspec_encoder *encoder;   /* Encoders/forms only; NULL if unavailable. */
} dsymspec_decl;

typedef struct dsymspec_target {
    const char *name;                  /* profile name, e.g. rv64im */
    size_t declaration_count;
    const dsymspec_decl *declarations;
} dsymspec_target;

static inline const dsymspec_decl *dsymspec_find(const dsymspec_target *target,
                                                dsymspec_decl_kind kind,
                                                const char *name) {
    size_t i;
    if (!target || !name) return NULL;
    for (i = 0; i < target->declaration_count; ++i) {
        const dsymspec_decl *decl = &target->declarations[i];
        if (decl->kind == kind && !strcmp(decl->name, name)) return decl;
    }
    return NULL;                      /* returns first match, e.g. an overloaded form */
}

typedef struct dsymspec_binding {
    const char *name;
    int64_t integer;                   /* used when string == NULL */
    const char *string;
} dsymspec_binding;

typedef struct dsymspec_relocation {
    size_t offset;                    /* relative to start of this encoding */
    const char *symbol;               /* borrowed from a binding or the recipe */
    const char *type_name;            /* NULL for numeric types */
    uint32_t type_id;                 /* valid only if type_name == NULL */
    int64_t addend;
} dsymspec_relocation;

typedef enum dsymspec_encode_status {
    DSYMSPEC_ENCODE_OK, DSYMSPEC_ENCODE_UNSUPPORTED, DSYMSPEC_ENCODE_BAD_ARGUMENT,
    DSYMSPEC_ENCODE_MISSING_BINDING, DSYMSPEC_ENCODE_WRONG_TYPE,
    DSYMSPEC_ENCODE_REJECTED, DSYMSPEC_ENCODE_RANGE, DSYMSPEC_ENCODE_CAPACITY,
    DSYMSPEC_ENCODE_INVALID_RECIPE
} dsymspec_encode_status;

/* Interpreter implementation. All code and generated data can be used from
 * either C11 or C++11 without linking Sandstone or an interpreter library.
 */
typedef struct dsymspec_detail_scalar {
    int64_t integer;
    const char *string;
} dsymspec_detail_scalar;

typedef struct dsymspec_detail_machine {
    const dsymspec_encoder *recipe;
    const dsymspec_binding *bindings;
    size_t binding_count;
    uint8_t *bytes;
    size_t capacity, size;
    dsymspec_relocation *relocs;
    size_t reloc_capacity, reloc_count;
    size_t steps;
    dsymspec_encode_status error;
} dsymspec_detail_machine;

static inline int dsymspec_detail_error(dsymspec_detail_machine *vm,
                                         dsymspec_encode_status error) {
    vm->error = error;
    return 0;
}

static inline int dsymspec_detail_expr(dsymspec_detail_machine *, uint32_t,
                                        unsigned, dsymspec_detail_scalar *);

static inline int dsymspec_detail_int(dsymspec_detail_machine *vm, uint32_t index,
                                       unsigned depth, int64_t *value) {
    dsymspec_detail_scalar item;
    if (!dsymspec_detail_expr(vm, index, depth, &item)) return 0;
    if (item.string) return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_WRONG_TYPE);
    *value = item.integer;
    return 1;
}

static inline int dsymspec_detail_expr(dsymspec_detail_machine *vm, uint32_t index,
                                        unsigned depth, dsymspec_detail_scalar *value) {
    const dsymspec_expr *expr;
    int64_t a, b;
    size_t i;
    if (index >= vm->recipe->expr_count || depth > 128 || ++vm->steps > 100000)
        return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_INVALID_RECIPE);
    expr = &vm->recipe->exprs[index];
    value->integer = 0;
    value->string = NULL;
    switch (expr->kind) {
    case DSYMSPEC_EXPR_INT:
        value->integer = expr->integer;
        return 1;
    case DSYMSPEC_EXPR_STRING:
        if (!expr->text) return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_INVALID_RECIPE);
        value->string = expr->text;
        return 1;
    case DSYMSPEC_EXPR_BINDING:
        if (!expr->text) return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_INVALID_RECIPE);
        for (i = 0; i < vm->binding_count; ++i) {
            if (!strcmp(vm->bindings[i].name, expr->text)) {
                value->integer = vm->bindings[i].integer;
                value->string = vm->bindings[i].string;
                return 1;
            }
        }
        return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_MISSING_BINDING);
    default: break;
    }
    if (!dsymspec_detail_int(vm, expr->args[0], depth + 1, &a)) return 0;
    switch (expr->kind) {
    case DSYMSPEC_EXPR_NEG:
        if (a == INT64_MIN) return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_RANGE);
        value->integer = -a;
        return 1;
    case DSYMSPEC_EXPR_NOT: value->integer = !a; return 1;
    case DSYMSPEC_EXPR_LOW8: value->integer = (int64_t)((uint64_t)a & 255); return 1;
    case DSYMSPEC_EXPR_SELECT:
        return dsymspec_detail_expr(vm, expr->args[a ? 1 : 2], depth + 1, value);
    case DSYMSPEC_EXPR_AND: if (!a) return 1; break;
    case DSYMSPEC_EXPR_OR: if (a) { value->integer = 1; return 1; } break;
    default: break;
    }
    if (!dsymspec_detail_int(vm, expr->args[1], depth + 1, &b)) return 0;
    switch (expr->kind) {
    case DSYMSPEC_EXPR_ADD:
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
            return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_RANGE);
        value->integer = a + b;
        break;
    case DSYMSPEC_EXPR_SUB:
        if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
            return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_RANGE);
        value->integer = a - b;
        break;
    case DSYMSPEC_EXPR_MUL:
        if ((a > 0 && ((b > 0 && a > INT64_MAX / b) || (b < 0 && b < INT64_MIN / a))) ||
            (a < 0 && ((b > 0 && a < INT64_MIN / b) || (b < 0 && a < INT64_MAX / b))))
            return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_RANGE);
        value->integer = a * b;
        break;
    case DSYMSPEC_EXPR_DIV:
    case DSYMSPEC_EXPR_MOD:
        if (!b || (a == INT64_MIN && b == -1))
            return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_RANGE);
        value->integer = expr->kind == DSYMSPEC_EXPR_DIV ? a / b : a % b;
        break;
    case DSYMSPEC_EXPR_EQ: value->integer = a == b; break;
    case DSYMSPEC_EXPR_NE: value->integer = a != b; break;
    case DSYMSPEC_EXPR_LT: value->integer = a < b; break;
    case DSYMSPEC_EXPR_LE: value->integer = a <= b; break;
    case DSYMSPEC_EXPR_GT: value->integer = a > b; break;
    case DSYMSPEC_EXPR_GE: value->integer = a >= b; break;
    case DSYMSPEC_EXPR_AND:
    case DSYMSPEC_EXPR_OR: value->integer = b != 0; break;
    default: return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_INVALID_RECIPE);
    }
    return 1;
}

static inline int dsymspec_detail_emit(dsymspec_detail_machine *vm, uint64_t bits,
                                        int64_t width, int big_endian) {
    size_t count, i;
    if (width != 1 && width != 2 && width != 4 && width != 8)
        return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_RANGE);
    count = (size_t)width;
    if (count > vm->recipe->max_bytes - vm->size)
        return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_RANGE);
    if (count > vm->capacity - vm->size)
        return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_CAPACITY);
    if (vm->bytes)
        for (i = 0; i < count; ++i)
            vm->bytes[vm->size + i] = (uint8_t)(bits >> (8 * (big_endian ? count - i - 1 : i)));
    vm->size += count;
    return 1;
}

static inline int dsymspec_detail_reloc(dsymspec_detail_machine *vm, const dsymspec_op *op) {
    dsymspec_detail_scalar symbol, type;
    dsymspec_relocation relocation;
    if (!dsymspec_detail_expr(vm, op->args[0], 0, &symbol)) return 0;
    if (!symbol.string) return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_WRONG_TYPE);
    relocation.offset = vm->size;
    relocation.symbol = symbol.string;
    relocation.type_name = op->text;
    relocation.type_id = op->kind == DSYMSPEC_OP_RELOC_ID ? (uint32_t)op->detail : 0;
    relocation.addend = 0;
    if (op->kind == DSYMSPEC_OP_RELOC_DYNAMIC) {
        if (!dsymspec_detail_expr(vm, op->args[1], 0, &type)) return 0;
        if (!type.string) return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_WRONG_TYPE);
        relocation.type_name = type.string;
    }
    if (op->args[2] != UINT32_MAX &&
        !dsymspec_detail_int(vm, op->args[2], 0, &relocation.addend)) return 0;
    if (vm->reloc_count >= vm->recipe->max_relocs)
        return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_RANGE);
    if (vm->reloc_count >= vm->reloc_capacity)
        return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_CAPACITY);
    if (vm->relocs) vm->relocs[vm->reloc_count] = relocation;
    ++vm->reloc_count;
    return 1;
}

static inline int dsymspec_detail_run(dsymspec_detail_machine *vm) {
    size_t pc = 0;
    while (pc < vm->recipe->op_count) {
        const dsymspec_op *op = &vm->recipe->ops[pc++];
        int64_t a, b, c;
        if (++vm->steps > 100000)
            return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_INVALID_RECIPE);
        switch (op->kind) {
        case DSYMSPEC_OP_EMIT:
            if (op->detail < -8 || op->detail > 8)
                return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_INVALID_RECIPE);
            if (!dsymspec_detail_int(vm, op->args[0], 0, &a) ||
                !dsymspec_detail_emit(vm, (uint64_t)a,
                                      op->detail < 0 ? -op->detail : op->detail,
                                      op->detail < 0)) return 0;
            break;
        case DSYMSPEC_OP_EMIT_SIZED:
            if (!dsymspec_detail_int(vm, op->args[0], 0, &a) ||
                !dsymspec_detail_int(vm, op->args[1], 0, &b) ||
                !dsymspec_detail_emit(vm, (uint64_t)b, a, 0)) return 0;
            break;
        case DSYMSPEC_OP_MODRM:
            if (!dsymspec_detail_int(vm, op->args[0], 0, &a) ||
                !dsymspec_detail_int(vm, op->args[1], 0, &b) ||
                !dsymspec_detail_emit(vm, 0xc0 | (((uint64_t)a & 7) << 3) | ((uint64_t)b & 7),
                                      1, 0)) return 0;
            break;
        case DSYMSPEC_OP_SIB:
            if (!dsymspec_detail_int(vm, op->args[0], 0, &a) ||
                !dsymspec_detail_int(vm, op->args[1], 0, &b) ||
                !dsymspec_detail_int(vm, op->args[2], 0, &c) ||
                !dsymspec_detail_emit(vm, (((uint64_t)a & 3) << 6) |
                                      (((uint64_t)b & 7) << 3) | ((uint64_t)c & 7), 1, 0)) return 0;
            break;
        case DSYMSPEC_OP_REQUIRE:
            if (!dsymspec_detail_int(vm, op->args[0], 0, &a)) return 0;
            if (!a) return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_REJECTED);
            break;
        case DSYMSPEC_OP_IF_FALSE:
            if (!dsymspec_detail_int(vm, op->args[0], 0, &a)) return 0;
            if (a) break;
            /* fall through */
        case DSYMSPEC_OP_JUMP:
            if (op->jump < pc || op->jump > vm->recipe->op_count)
                return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_INVALID_RECIPE);
            pc = op->jump;
            break;
        case DSYMSPEC_OP_RELOC_NAME:
        case DSYMSPEC_OP_RELOC_ID:
        case DSYMSPEC_OP_RELOC_DYNAMIC:
            if (!dsymspec_detail_reloc(vm, op)) return 0;
            break;
        case DSYMSPEC_OP_FAIL: return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_REJECTED);
        default: return dsymspec_detail_error(vm, DSYMSPEC_ENCODE_INVALID_RECIPE);
        }
    }
    return 1;
}

/* Encode using Sandstone's integer expression/statement semantics. The caller
 * selects a form and normalizes its operands into named bindings. This is not
 * an assembler, does not select by feature/syntax, and does not apply relocations.
 * A relocation's offset is the position at the emit_relocation operation.
 * Bindings, output arrays, and size pointers must not overlap. On failure the
 * sizes are zero and the output arrays are unchanged.
 */
static inline dsymspec_encode_status
dsymspec_encode(const dsymspec_encoder *recipe, const dsymspec_binding *bindings,
                size_t binding_count, uint8_t *bytes, size_t capacity, size_t *size,
                dsymspec_relocation *relocs, size_t reloc_capacity, size_t *reloc_count) {
    dsymspec_detail_machine vm;
    size_t i;
    if (size) *size = 0;
    if (reloc_count) *reloc_count = 0;
    if (!recipe || recipe->unsupported_reason) return DSYMSPEC_ENCODE_UNSUPPORTED;
    if ((!bindings && binding_count) || (!bytes && capacity) || (!relocs && reloc_capacity))
        return DSYMSPEC_ENCODE_BAD_ARGUMENT;
    if ((!recipe->exprs && recipe->expr_count) || (!recipe->ops && recipe->op_count))
        return DSYMSPEC_ENCODE_INVALID_RECIPE;
    for (i = 0; i < binding_count; ++i)
        if (!bindings[i].name) return DSYMSPEC_ENCODE_BAD_ARGUMENT;
    vm.recipe = recipe; vm.bindings = bindings; vm.binding_count = binding_count;
    vm.bytes = NULL; vm.capacity = capacity; vm.size = 0;
    vm.relocs = NULL; vm.reloc_capacity = reloc_capacity; vm.reloc_count = 0;
    vm.steps = 0; vm.error = DSYMSPEC_ENCODE_OK;
    if (!dsymspec_detail_run(&vm)) return vm.error;
    vm.bytes = bytes; vm.relocs = relocs;
    vm.size = 0; vm.reloc_count = 0; vm.steps = 0;
    if (!dsymspec_detail_run(&vm)) return vm.error;
    if (size) *size = vm.size;
    if (reloc_count) *reloc_count = vm.reloc_count;
    return DSYMSPEC_ENCODE_OK;
}

#endif
