#include "TypedTACBackend.h"

#include <stdio.h>
#include <stdlib.h>

/* Bound the legacy verifier before any of its table walks. Besides linear
 * table checks, it searches for string terminators and checks phi predecessor
 * sets with nested loops. Bound those costs independently of declared capacity. */
enum {
    TAC_MAX_BLOCKS = 256,
    TAC_MAX_VALUES = 4096,
    TAC_MAX_PARAMS = 64,
    TAC_MAX_OPERATIONS = TAC_MAX_VALUES + TAC_MAX_BLOCKS,
    TAC_MAX_INPUTS = 16384
};
#define TAC_MAX_STRING_BYTES ((size_t)64 * 1024)
#define TAC_MAX_VERIFY_WORK ((size_t)64 * 1024 * 1024)

typedef struct {
    ArchonMIRBlock *blocks;
    ArchonMIRInst *insts;
    ArchonMIRType *types;
    ArchonValue *values, *params, *args;
} TACMIR;

static ArchonBackendStatus tac_error(ArchonBackendDiagnostic *diagnostic,
    ArchonBackendStatus status, ArchonBackendStage stage,
    size_t block, size_t operation, const char *message)
{
    if (diagnostic) {
        *diagnostic = (ArchonBackendDiagnostic){
            .status = status, .stage = stage,
            .function = stage == ARCHON_BACKEND_VERIFY ? 0 : SIZE_MAX,
            .block = block, .instruction = operation
        };
        (void)snprintf(diagnostic->message, sizeof diagnostic->message, "%s", message);
    }
    return status;
}

static int tac_cancelled(const ArchonBackendOptions *options)
{
    return options && options->cancelled && options->cancelled(options->user);
}

static ArchonBackendStatus tac_preflight(const IR_Program *program,
    ArchonBackendDiagnostic *diagnostic)
{
    size_t i, names, work = TAC_MAX_VERIFY_WORK;
    if (program->block_count > TAC_MAX_BLOCKS ||
        program->temporary_count > TAC_MAX_VALUES ||
        program->operation_count > TAC_MAX_OPERATIONS ||
        program->input_count > TAC_MAX_INPUTS ||
        program->string_size > TAC_MAX_STRING_BYTES)
        return tac_error(diagnostic, ARCHON_BACKEND_LIMIT, ARCHON_BACKEND_VERIFY,
            SIZE_MAX, SIZE_MAX, "typed TAC table limit");
    if (program->failed || !program->block_count ||
        program->block_count > program->block_capacity || !program->blocks ||
        program->temporary_count > program->temporary_capacity ||
        (program->temporary_count && !program->temporaries) ||
        program->operation_count > program->operation_capacity ||
        (program->operation_count && !program->operations) ||
        program->input_count > program->input_capacity ||
        (program->input_count && !program->inputs) ||
        program->string_size > program->string_capacity ||
        (program->string_size && !program->strings))
        return tac_error(diagnostic, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_VERIFY,
            SIZE_MAX, SIZE_MAX, "invalid typed TAC program or table storage");

    names = program->block_count + program->temporary_count + program->operation_count;
    work -= names + program->input_count;
    if (program->string_size && names > work / program->string_size)
        return tac_error(diagnostic, ARCHON_BACKEND_LIMIT, ARCHON_BACKEND_VERIFY,
            SIZE_MAX, SIZE_MAX, "typed TAC verification work limit (strings)");
    work -= names * program->string_size;

    for (i = 0; i < program->operation_count; ++i) {
        const IR_Operation *op = &program->operations[i];
        size_t n = op->input_count, cost;
        if (op->kind != IR_OP_PHI)
            continue;
        if (n > program->block_count)
            return tac_error(diagnostic, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_VERIFY,
                SIZE_MAX, i, "too many inputs for a typed TAC phi");
        /* Duplicate checking, followed by a scan of all potential predecessors
         * and an incoming-list lookup for each actual predecessor. */
        cost = n * n + program->block_count * (n + 1);
        if (cost > work)
            return tac_error(diagnostic, ARCHON_BACKEND_LIMIT, ARCHON_BACKEND_VERIFY,
                SIZE_MAX, i, "typed TAC verification work limit (phi predecessors)");
        work -= cost;
    }
    return ARCHON_BACKEND_OK;
}

static int tac_word(IR_Type type)
{
    return type.width == 64 &&
        (type.kind == IR_TYPE_BITS || type.kind == IR_TYPE_SINT || type.kind == IR_TYPE_UINT);
}

/* Only admit operations whose word semantics agree with MIR on all input
 * bit patterns. For example, MIR masks shift counts; TAC does not specify
 * what out-of-range counts do, so even word shifts are not admitted. */
static ArchonMIROpcode tac_primitive(const IR_Program *program, const IR_Operation *op)
{
    switch (op->kind) {
    case IR_OP_CONSTANT: return ARCHON_MIR_CONST;
    case IR_OP_COPY: return ARCHON_MIR_COPY;
    case IR_OP_EQ: return ARCHON_MIR_EQ;
    case IR_OP_LT: {
        const IR_Input *left = &program->inputs[op->first_input];
        IR_Type type = program->temporaries[left->temp].type;
        if (tac_word(type)) {
            if (type.kind == IR_TYPE_SINT)
                return ARCHON_MIR_SLT;
            if (type.kind == IR_TYPE_UINT)
                return ARCHON_MIR_ULT;
        }
        return 0;
    }
    default: break;
    }
    if (!tac_word(op->type))
        return 0;
    switch (op->kind) {
    case IR_OP_ADD: return ARCHON_MIR_ADD;
    case IR_OP_SUB: return ARCHON_MIR_SUB;
    case IR_OP_MUL: return ARCHON_MIR_MUL;
    case IR_OP_AND: return ARCHON_MIR_AND;
    case IR_OP_OR: return ARCHON_MIR_OR;
    case IR_OP_XOR: return ARCHON_MIR_XOR;
    default: return 0;
    }
}

static ArchonBackendStatus tac_build(const IR_Program *program,
    const ArchonBackendOptions *options, TACMIR *mir, ArchonMIRFunction *function,
    ArchonBackendDiagnostic *diagnostic)
{
    size_t b, k, value = 0, param = 0, inst = 0;
    IR_Type result = { .kind = IR_TYPE_VOID };
    int has_return = 0;

    for (b = 0; b < program->block_count; ++b) {
        const IR_BasicBlock *bb = &program->blocks[b];
        ArchonMIRBlock *block = &mir->blocks[b];
        if (tac_cancelled(options))
            return tac_error(diagnostic, ARCHON_BACKEND_CANCELLED, ARCHON_BACKEND_VERIFY,
                b, SIZE_MAX, "native compilation cancelled");
        block->params = mir->params + param;
        block->insts = mir->insts + inst;
        for (k = 0; k < bb->operation_count; ++k) {
            size_t index = bb->first_operation + k;
            const IR_Operation *op = &program->operations[index];

            if (op->effect_in != IR_TEMP_NONE || op->effect_out != IR_TEMP_NONE ||
                op->kind == IR_OP_EFFECT_START || op->kind == IR_OP_CALL ||
                op->kind == IR_OP_LOAD || op->kind == IR_OP_STORE ||
                op->type.kind == IR_TYPE_EFFECT)
                return tac_error(diagnostic, ARCHON_BACKEND_UNSUPPORTED, ARCHON_BACKEND_VERIFY,
                    b, index, "typed TAC native adapter does not support effects, calls or memory");
            if (op->kind != IR_OP_CONSTANT && op->immediate)
                return tac_error(diagnostic, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_VERIFY,
                    b, index, "unexpected typed TAC immediate");
            if (op->result != IR_TEMP_NONE) {
                if (!tac_word(op->type) && op->type.kind != IR_TYPE_BOOL)
                    return tac_error(diagnostic, ARCHON_BACKEND_UNSUPPORTED, ARCHON_BACKEND_VERIFY,
                        b, index, "typed TAC native adapter requires a 64-bit scalar or internal bool");
                mir->values[op->result] = (ArchonValue)value;
                mir->types[value++] = ARCHON_MIR_I64;
            }

            switch (op->kind) {
            case IR_OP_PARAMETER:
                if (b || k != function->param_count)
                    return tac_error(diagnostic, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_VERIFY,
                        b, index, "typed TAC parameters must precede entry instructions");
                if (!tac_word(op->type))
                    return tac_error(diagnostic, ARCHON_BACKEND_UNSUPPORTED, ARCHON_BACKEND_VERIFY,
                        b, index, "typed TAC ABI parameters must be 64-bit scalars");
                if (function->param_count == TAC_MAX_PARAMS)
                    return tac_error(diagnostic, ARCHON_BACKEND_LIMIT, ARCHON_BACKEND_VERIFY,
                        b, index, "typed TAC ABI parameter limit");
                ++function->param_count;
                break;
            case IR_OP_PHI:
                if (!b || k != block->param_count)
                    return tac_error(diagnostic, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_VERIFY,
                        b, index, "typed TAC phis must precede non-entry instructions");
                mir->params[param++] = mir->values[op->result];
                ++block->param_count;
                break;
            case IR_OP_BRANCH:
                block->term = (ArchonMIRTerm){ .kind = ARCHON_MIR_BR,
                    .value = ARCHON_MIR_NONE, .yes.target = op->target_true };
                break;
            case IR_OP_COND_BRANCH:
                block->term = (ArchonMIRTerm){ .kind = ARCHON_MIR_CBR,
                    .value = program->inputs[op->first_input].temp,
                    .yes.target = op->target_true, .no.target = op->target_false };
                break;
            case IR_OP_RETURN: {
                IR_Type type = { .kind = IR_TYPE_VOID };
                IR_Temp returned = IR_TEMP_NONE;
                if (op->input_count) {
                    returned = program->inputs[op->first_input].temp;
                    type = program->temporaries[returned].type;
                    if (!tac_word(type))
                        return tac_error(diagnostic, ARCHON_BACKEND_UNSUPPORTED, ARCHON_BACKEND_VERIFY,
                            b, index, "typed TAC ABI returns must be void or 64-bit scalars");
                }
                if (has_return && !ir_type_equal(result, type))
                    return tac_error(diagnostic, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_VERIFY,
                        b, index, "typed TAC return types differ");
                has_return = 1;
                result = type;
                block->term = (ArchonMIRTerm){ .kind = ARCHON_MIR_RETURN,
                    .value = returned == IR_TEMP_NONE ? ARCHON_MIR_NONE : returned };
                break;
            }
            default: {
                ArchonMIROpcode opcode = tac_primitive(program, op);
                if (!opcode)
                    return tac_error(diagnostic, ARCHON_BACKEND_UNSUPPORTED, ARCHON_BACKEND_VERIFY,
                        b, index, "typed TAC operation has no exact native primitive for its type");
                mir->insts[inst++] = (ArchonMIRInst){
                    .op = opcode, .dst = mir->values[op->result], .imm = op->immediate,
                    /* Remap inputs after assigning all IDs. A dominating
                     * definition can be later in block layout order. */
                    .a = op->input_count ? program->inputs[op->first_input].temp
                                         : ARCHON_MIR_NONE,
                    .b = op->input_count == 2 ? program->inputs[op->first_input + 1].temp
                                              : ARCHON_MIR_NONE
                };
                ++block->inst_count;
                break;
            }
            }
        }
    }
    function->result = result.kind == IR_TYPE_VOID ? ARCHON_MIR_VOID : ARCHON_MIR_I64;
    return ARCHON_BACKEND_OK;
}

static int tac_edge(const IR_Program *program, TACMIR *mir, IR_Block predecessor,
    ArchonMIREdge *edge, size_t *offset)
{
    const IR_BasicBlock *target = &program->blocks[edge->target];
    size_t k, count = mir->blocks[edge->target].param_count;
    if (!count)
        return 1;
    /* Each distinct edge argument consumes one verified TAC phi input. */
    if (count > program->input_count - *offset)
        return 0;
    edge->arg_count = count;
    edge->args = mir->args + *offset;
    for (k = 0; k < count; ++k) {
        const IR_Operation *phi = &program->operations[target->first_operation + k];
        const IR_Input *inputs = &program->inputs[phi->first_input];
        size_t j;
        for (j = 0; j < phi->input_count && inputs[j].predecessor != predecessor; ++j)
            ;
        if (j == phi->input_count)
            return 0; /* Structural verification already checked completeness. */
        mir->args[(*offset)++] = mir->values[inputs[j].temp];
    }
    return 1;
}

static ArchonBackendStatus tac_connect(const IR_Program *program,
    const ArchonBackendOptions *options, TACMIR *mir, ArchonBackendDiagnostic *diagnostic)
{
    size_t b, offset = 0, inst = 0;
    for (b = 0; b < program->block_count; ++b) {
        ArchonMIRBlock *block = &mir->blocks[b];
        ArchonMIRTerm *term = &block->term;
        size_t j;
        if (tac_cancelled(options))
            return tac_error(diagnostic, ARCHON_BACKEND_CANCELLED, ARCHON_BACKEND_VERIFY,
                b, SIZE_MAX, "native compilation cancelled");
        for (j = 0; j < block->inst_count; ++j) {
            ArchonMIRInst *op = &mir->insts[inst++];
            if (op->a != ARCHON_MIR_NONE)
                op->a = mir->values[op->a];
            if (op->b != ARCHON_MIR_NONE)
                op->b = mir->values[op->b];
        }
        if (term->value != ARCHON_MIR_NONE)
            term->value = mir->values[term->value];
        if (term->kind == ARCHON_MIR_RETURN)
            continue;
        if (!tac_edge(program, mir, (IR_Block)b, &term->yes, &offset))
            break;
        if (term->kind == ARCHON_MIR_CBR) {
            /* TAC keys phis by predecessor, not by conditional arm. */
            if (term->no.target == term->yes.target)
                term->no = term->yes;
            else if (!tac_edge(program, mir, (IR_Block)b, &term->no, &offset))
                break;
        }
    }
    if (b != program->block_count)
        return tac_error(diagnostic, ARCHON_BACKEND_INTERNAL, ARCHON_BACKEND_VERIFY,
            b, SIZE_MAX, "failed to map verified typed TAC edge arguments");
    return ARCHON_BACKEND_OK;
}

ArchonBackendStatus archon_typed_tac_backend_compile_ex(
    const IR_Program *program, const char *symbol,
    const ArchonBackendOptions *options, unsigned flags, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic)
{
    IR_Diagnostic ir_diagnostic;
    TACMIR mir;
    ArchonMIRFunction function = {0};
    ArchonMIRModule module = { .version = ARCHON_MIR_SCHEMA_VERSION,
        .functions = &function, .function_count = 1 };
    ArchonBackendStatus status;
    size_t values, args;

    if (!program || !symbol || !artifact || artifact->data || artifact->size)
        return tac_error(diagnostic, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_INPUT,
            SIZE_MAX, SIZE_MAX, "a typed TAC program, symbol, and empty artifact are required");
    if (tac_cancelled(options))
        return tac_error(diagnostic, ARCHON_BACKEND_CANCELLED, ARCHON_BACKEND_INPUT,
            SIZE_MAX, SIZE_MAX, "native compilation cancelled");
    status = tac_preflight(program, diagnostic);
    if (status != ARCHON_BACKEND_OK)
        return status;
    if (!ir_verify(program, &ir_diagnostic)) {
        size_t index = ir_diagnostic.operation, block = SIZE_MAX;
        if (index < program->operation_count &&
            program->operations[index].block < program->block_count)
            block = program->operations[index].block;
        return tac_error(diagnostic, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_VERIFY,
            block, index, ir_diagnostic.message);
    }
    if (tac_cancelled(options))
        return tac_error(diagnostic, ARCHON_BACKEND_CANCELLED, ARCHON_BACKEND_VERIFY,
            SIZE_MAX, SIZE_MAX, "native compilation cancelled");

    values = program->temporary_count ? program->temporary_count : 1;
    args = program->input_count ? program->input_count : 1;
    mir.blocks = calloc(program->block_count, sizeof *mir.blocks);
    mir.insts = malloc(values * sizeof *mir.insts);
    mir.types = malloc(values * sizeof *mir.types);
    mir.values = malloc(values * sizeof *mir.values);
    mir.params = malloc(values * sizeof *mir.params);
    mir.args = malloc(args * sizeof *mir.args);
    if (!mir.blocks || !mir.insts || !mir.types || !mir.values || !mir.params || !mir.args) {
        status = tac_error(diagnostic, ARCHON_BACKEND_NOMEM, ARCHON_BACKEND_INPUT,
            SIZE_MAX, SIZE_MAX, "cannot allocate typed TAC adapter MachineIR tables");
        goto done;
    }
    function.name = symbol;
    function.blocks = mir.blocks;
    function.block_count = program->block_count;
    function.types = mir.types;
    function.value_count = program->temporary_count;
    status = tac_build(program, options, &mir, &function, diagnostic);
    if (status == ARCHON_BACKEND_OK)
        status = tac_connect(program, options, &mir, diagnostic);
    if (status == ARCHON_BACKEND_OK)
        status = archon_backend_compile_ex(&module, options, flags, artifact, diagnostic);

done:
    free(mir.args);
    free(mir.params);
    free(mir.values);
    free(mir.types);
    free(mir.insts);
    free(mir.blocks);
    return status;
}

ArchonBackendStatus archon_typed_tac_backend_compile(
    const IR_Program *program, const char *symbol,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic)
{
    return archon_typed_tac_backend_compile_ex(program, symbol, options, 0, artifact, diagnostic);
}
