#include "NativeInternal.h"

#include <stdlib.h>
#include <string.h>

enum {
    AN_DOM_WORDS = (AN_MAX_BLOCK + 63u) / 64u,
    AN_MAX_EDGES = 2u * AN_MAX_BLOCK,
    HAS_DST = 1u, HAS_A = 2u, HAS_B = 4u
};

#define AN_MAX_VERIFY_WORK (64u * AN_MAX_MODULE_INSTS)
#define AN_MAX_GLOBAL_WORDS 65536u

typedef enum { SYM_FUNCTION, SYM_IMPORT, SYM_GLOBAL } SymbolKind;
typedef struct {
    const char *name;
    SymbolKind kind;
    size_t index;
} Symbol;
typedef struct {
    const ArchonMIRModule *module;
    Symbol symbols[AN_MAX_SYMBOLS];
    size_t symbol_count, work_left, insts_left, words_left;
} VerifyModule;

typedef struct {
    uint16_t block; /* One-based; zero means undefined. */
    uint16_t position; /* Zero for a parameter, otherwise instruction index + 1. */
} Definition;
typedef struct {
    unsigned from, next;
} Predecessor;
typedef struct {
    Definition defs[AN_MAX_VALUE];
    uint64_t *dom;
    unsigned dom_words;
    unsigned order[AN_MAX_BLOCK];
    unsigned char seen[AN_MAX_BLOCK];
    unsigned incoming[AN_MAX_BLOCK];
    Predecessor preds[AN_MAX_EDGES + 1u]; /* Index 0 terminates a list. */
    unsigned pred_count;
} VerifyFunction;

static uint64_t *dom_set(const VerifyFunction *v, size_t block)
{
    return v->dom + block * v->dom_words;
}

static int result_type(ArchonMIRType type)
{
    return type == ARCHON_MIR_VOID || an_is_value_type(type);
}

static int array_ok(ANContext *ctx, const void *array, size_t count,
    size_t limit, const char *message)
{
    if (count > limit)
        return an_error(ctx, ARCHON_BACKEND_LIMIT, message);
    if (count && !array)
        return an_error(ctx, ARCHON_BACKEND_INVALID, message);
    return 1;
}

static int charge(ANContext *ctx, size_t *remaining, size_t units,
    const char *message)
{
    if (units > *remaining)
        return an_error(ctx, ARCHON_BACKEND_LIMIT, message);
    *remaining -= units;
    return 1;
}

static int work(ANContext *ctx, VerifyModule *m, size_t units)
{
    return charge(ctx, &m->work_left, units, "module verification work limit");
}

static size_t symbol_position(const VerifyModule *m, const char *name)
{
    size_t lo = 0, hi = m->symbol_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(m->symbols[mid].name, name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static const Symbol *find_symbol(const VerifyModule *m, const char *name)
{
    size_t pos = symbol_position(m, name);
    return pos < m->symbol_count && !strcmp(m->symbols[pos].name, name)
        ? &m->symbols[pos] : NULL;
}

static int add_symbol(ANContext *ctx, VerifyModule *m, const char *name,
    SymbolKind kind, size_t index)
{
    size_t pos;
    if (!an_symbol(name))
        return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid symbol name");
    pos = symbol_position(m, name);
    if (pos < m->symbol_count && !strcmp(m->symbols[pos].name, name))
        return an_error(ctx, ARCHON_BACKEND_INVALID, "duplicate module symbol");
    if (pos < m->symbol_count)
        memmove(&m->symbols[pos + 1], &m->symbols[pos],
            (m->symbol_count - pos) * sizeof(m->symbols[0]));
    m->symbols[pos] = (Symbol){ name, kind, index };
    ++m->symbol_count;
    return 1;
}

static int verify_declarations(ANContext *ctx, VerifyModule *m)
{
    const ArchonMIRModule *module = m->module;
    size_t i, j;

    for (i = 0; i < module->function_count; ++i) {
        const ArchonMIRFunction *fn = &module->functions[i];
        ctx->function = i;
        if (!an_continue(ctx) || !add_symbol(ctx, m, fn->name, SYM_FUNCTION, i) ||
            !array_ok(ctx, fn->types, fn->value_count, AN_MAX_VALUE, "invalid value table") ||
            !array_ok(ctx, fn->blocks, fn->block_count, AN_MAX_BLOCK, "invalid block table") ||
            !array_ok(ctx, fn->locals, fn->local_count, AN_MAX_VALUE, "invalid local table"))
            return 0;
        if (fn->param_count > AN_MAX_PARAMS)
            return an_error(ctx, ARCHON_BACKEND_LIMIT, "function parameter limit");
        if (!fn->block_count || fn->param_count > fn->value_count || !result_type(fn->result))
            return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid function declaration");
        if (!an_target_type(ctx, fn->result) ||
            !work(ctx, m, fn->value_count + fn->block_count + fn->local_count))
            return 0;
        for (j = 0; j < fn->value_count; ++j) {
            if (!an_is_value_type(fn->types[j]))
                return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid value type");
            if (!an_target_type(ctx, fn->types[j]))
                return 0;
        }
        for (j = 0; j < fn->local_count; ++j) {
            const ArchonMIRLocal *local = &fn->locals[j];
            uint32_t align = local->alignment;
            if (!local->size || !align || (align & (align - 1u)))
                return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid local size or alignment");
            if (align > an_abi.stack_align)
                return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "local exceeds ABI stack alignment");
        }
    }
    ctx->function = SIZE_MAX;
    for (i = 0; i < module->import_count; ++i) {
        const ArchonMIRImport *import = &module->imports[i];
        if (!an_continue(ctx) || !add_symbol(ctx, m, import->name, SYM_IMPORT, i) ||
            !array_ok(ctx, import->params, import->param_count, AN_MAX_PARAMS,
                "invalid import parameter table"))
            return 0;
        if (!result_type(import->result))
            return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid import result type");
        if (!an_target_type(ctx, import->result))
            return 0;
        for (j = 0; j < import->param_count; ++j) {
            if (!an_is_value_type(import->params[j]))
                return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid import parameter type");
            if (!an_target_type(ctx, import->params[j]))
                return 0;
        }
    }
    for (i = 0; i < module->global_count; ++i) {
        const ArchonMIRGlobal *global = &module->globals[i];
        if (!an_continue(ctx) || !add_symbol(ctx, m, global->name, SYM_GLOBAL, i) ||
            !charge(ctx, &m->words_left, global->word_count, "module global word limit"))
            return 0;
    }
    return 1;
}

static int define(ANContext *ctx, const ArchonMIRFunction *fn, VerifyFunction *v,
    ArchonValue value, unsigned position)
{
    if (value >= fn->value_count)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "definition value out of range");
    if (v->defs[value].block)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "duplicate SSA definition");
    v->defs[value] = (Definition){ (uint16_t)(ctx->block + 1u), (uint16_t)position };
    return 1;
}

/* The terminator kind is validated before all subsequent calls. */
static unsigned edge_count(const ArchonMIRTerm *term)
{
    return term->kind == ARCHON_MIR_CBR ? 2u : term->kind == ARCHON_MIR_BR ? 1u : 0u;
}

static const ArchonMIREdge *edge_at(const ArchonMIRTerm *term, unsigned i)
{
    return i ? &term->no : &term->yes;
}

static int collect(ANContext *ctx, VerifyModule *m, const ArchonMIRFunction *fn,
    VerifyFunction *v)
{
    size_t i, j, insts_left = AN_MAX_INSTS;

    for (i = 0; i < fn->param_count; ++i)
        v->defs[i] = (Definition){ 1u, 0u };
    for (i = 0; i < fn->block_count; ++i) {
        const ArchonMIRBlock *bb = &fn->blocks[i];
        const ArchonMIRTerm *term = &bb->term;
        ctx->block = i;
        ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx) ||
            !array_ok(ctx, bb->params, bb->param_count, AN_MAX_VALUE, "invalid block parameter table") ||
            !array_ok(ctx, bb->insts, bb->inst_count, AN_MAX_INSTS, "invalid instruction table") ||
            !charge(ctx, &insts_left, bb->inst_count, "function instruction limit") ||
            !charge(ctx, &m->insts_left, bb->inst_count, "module instruction limit") ||
            !work(ctx, m, bb->inst_count + bb->param_count))
            return 0;
        if (!i && bb->param_count)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "entry block has block parameters");
        for (j = 0; j < bb->param_count; ++j)
            if (!define(ctx, fn, v, bb->params[j], 0))
                return 0;
        for (j = 0; j < bb->inst_count; ++j) {
            const ArchonMIRInst *inst = &bb->insts[j];
            unsigned shape;
            ctx->instruction = j;
            if (!an_continue(ctx))
                return 0;
            shape = an_shape(inst->op);
            if (!shape)
                return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "unknown instruction opcode");
            /* Later passes can rely on all unused value fields being absent. */
            if ((!(shape & HAS_DST) && inst->dst != ARCHON_MIR_NONE) ||
                (!(shape & HAS_A) && inst->a != ARCHON_MIR_NONE) ||
                (!(shape & HAS_B) && inst->b != ARCHON_MIR_NONE))
                return an_error(ctx, ARCHON_BACKEND_INVALID, "unexpected fixed value operand");
            if ((shape & HAS_DST) && (!an_is_call(inst->op) || inst->dst != ARCHON_MIR_NONE) &&
                !define(ctx, fn, v, inst->dst, (unsigned)j + 1u))
                return 0;
        }
        ctx->instruction = SIZE_MAX;
        switch (term->kind) {
        case ARCHON_MIR_RETURN:
        case ARCHON_MIR_TRAP:
        case ARCHON_MIR_BR:
        case ARCHON_MIR_CBR:
            break;
        default:
            return an_error(ctx, ARCHON_BACKEND_INVALID, "unknown terminator");
        }
        if ((term->kind == ARCHON_MIR_BR || term->kind == ARCHON_MIR_TRAP) &&
            term->value != ARCHON_MIR_NONE)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "unexpected terminator value");
        for (j = edge_count(term); j < 2; ++j) {
            const ArchonMIREdge *e = edge_at(term, (unsigned)j);
            if (e->args || e->arg_count)
                return an_error(ctx, ARCHON_BACKEND_INVALID, "unexpected inactive edge arguments");
        }
        for (j = 0; j < edge_count(term); ++j) {
            const ArchonMIREdge *e = edge_at(term, (unsigned)j);
            unsigned pred;
            if (!e->target || e->target >= fn->block_count)
                return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid branch target or edge to entry");
            if (!array_ok(ctx, e->args, e->arg_count, AN_MAX_VALUE, "invalid edge argument table") ||
                !work(ctx, m, e->arg_count))
                return 0;
            pred = ++v->pred_count;
            v->preds[pred] = (Predecessor){ (unsigned)i, v->incoming[e->target] };
            v->incoming[e->target] = pred;
        }
    }
    return 1;
}

static int dominators(ANContext *ctx, VerifyModule *m, const ArchonMIRFunction *fn,
    VerifyFunction *v)
{
    unsigned *order = v->order;
    unsigned char *seen = v->seen;
    unsigned n = (unsigned)fn->block_count, tail = 1, i, j, pass;
    unsigned words = v->dom_words;

    ctx->instruction = SIZE_MAX;
    seen[0] = 1;
    for (i = 0; i < tail; ++i) {
        const ArchonMIRTerm *term = &fn->blocks[order[i]].term;
        ctx->block = order[i];
        if (!an_continue(ctx))
            return 0;
        for (j = 0; j < edge_count(term); ++j) {
            unsigned target = edge_at(term, j)->target;
            if (!seen[target]) {
                seen[target] = 1;
                order[tail++] = target;
            }
        }
    }
    if (tail != n) {
        for (i = 0; i < n && seen[i]; ++i)
            ;
        ctx->block = i;
        return an_error(ctx, ARCHON_BACKEND_INVALID, "block unreachable from entry");
    }

    ctx->block = SIZE_MAX;
    v->dom[0] = 1;
    for (i = 1; i < n; ++i)
        for (j = 0; j < words; ++j)
            dom_set(v, i)[j] = UINT64_MAX;
    /* Intersect predecessor sets in reachability order. If d doesn't dominate
     * b, a simple path to b avoiding d witnesses this within n-1 rounds. */
    for (pass = 0; pass < n; ++pass) {
        int changed = 0;
        if (!an_continue(ctx) || !work(ctx, m, (n + v->pred_count) * words))
            return 0;
        for (i = 1; i < n; ++i) {
            unsigned b = order[i], pred;
            uint64_t *set = dom_set(v, b);
            uint64_t bits[AN_DOM_WORDS];
            for (j = 0; j < words; ++j)
                bits[j] = UINT64_MAX;
            for (pred = v->incoming[b]; pred; pred = v->preds[pred].next)
                for (j = 0; j < words; ++j)
                    bits[j] &= dom_set(v, v->preds[pred].from)[j];
            bits[b / 64u] |= UINT64_C(1) << (b % 64u);
            for (j = 0; j < words; ++j) {
                changed |= set[j] != bits[j];
                set[j] = bits[j];
            }
        }
        if (!changed)
            return 1;
    }
    return an_error(ctx, ARCHON_BACKEND_INTERNAL, "dominance did not converge");
}

static int use(ANContext *ctx, const ArchonMIRFunction *fn, const VerifyFunction *v,
    ArchonValue value, unsigned position)
{
    const Definition *def;
    unsigned b;
    if (value >= fn->value_count)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "operand value out of range");
    def = &v->defs[value];
    if (!def->block)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "operand has no definition");
    b = def->block - 1u;
    if (b == ctx->block ? def->position >= position
        : !(dom_set(v, ctx->block)[b / 64u] & (UINT64_C(1) << (b % 64u))))
        return an_error(ctx, ARCHON_BACKEND_INVALID, "definition does not dominate use");
    return 1;
}

typedef struct {
    ArchonMIRType result;
    const ArchonMIRType *params;
    size_t arity;
} Signature;

static Signature signature(const VerifyModule *m, const Symbol *symbol)
{
    if (symbol->kind == SYM_FUNCTION) {
        const ArchonMIRFunction *f = &m->module->functions[symbol->index];
        return (Signature){f->result, f->types, f->param_count};
    } else {
        const ArchonMIRImport *f = &m->module->imports[symbol->index];
        return (Signature){f->result, f->params, f->param_count};
    }
}

/* Runtime-supplied pointers and block-parameter merges carry a signature
 * promise from the producer. For known function addresses (including COPY
 * chains), independently reject any statically visible disagreement. */
static int verify_indirect_signature(ANContext *ctx, VerifyModule *m,
    const ArchonMIRFunction *fn, const VerifyFunction *v, ArchonValue value,
    Signature expected)
{
    size_t steps;
    for (steps = 0; steps < fn->value_count; ++steps) {
        Definition def;
        const ArchonMIRInst *source;
        if (!work(ctx, m, 1))
            return 0;
        if (value >= fn->value_count)
            return 1; /* Invalid source use is diagnosed by the SSA verifier. */
        def = v->defs[value];
        if (!def.block || !def.position)
            return 1;
        source = &fn->blocks[def.block - 1].insts[def.position - 1];
        if (source->op == ARCHON_MIR_COPY)
            value = source->a;
        else if (source->op == ARCHON_MIR_FUNC_ADDR && an_symbol(source->symbol)) {
            const Symbol *symbol = find_symbol(m, source->symbol);
            Signature actual;
            size_t i;
            if (!symbol || symbol->kind == SYM_GLOBAL)
                return 1; /* Invalid address symbols are diagnosed separately. */
            actual = signature(m, symbol);
            if (actual.result != expected.result || actual.arity != expected.arity)
                goto mismatch;
            for (i = 0; i < actual.arity; ++i)
                if (actual.params[i] != expected.params[i])
                    goto mismatch;
            return 1;
        } else {
            return 1;
        }
    }
    return an_error(ctx, ARCHON_BACKEND_INVALID, "cyclic indirect target definition");
mismatch:
    return an_error(ctx, ARCHON_BACKEND_INVALID, "known indirect target has a different signature");
}

static int verify_call(ANContext *ctx, VerifyModule *m, const ArchonMIRFunction *fn,
    const VerifyFunction *v, const ArchonMIRInst *inst, unsigned position)
{
    const Symbol *symbol;
    Signature sig;
    size_t i;
    if (!array_ok(ctx, inst->args, inst->arg_count, AN_MAX_PARAMS, "invalid call argument table") ||
        !work(ctx, m, inst->arg_count))
        return 0;
    if (!an_symbol(inst->symbol))
        return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid call symbol");
    symbol = find_symbol(m, inst->symbol);
    if (!symbol || symbol->kind == SYM_GLOBAL)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "call target must be a declared function or import");
    sig = signature(m, symbol);
    if (sig.arity != inst->arg_count)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "call argument count does not match callee");
    if (sig.result == ARCHON_MIR_VOID ? inst->dst != ARCHON_MIR_NONE :
        inst->dst >= fn->value_count || fn->types[inst->dst] != sig.result)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "call result does not match callee");
    for (i = 0; i < sig.arity; ++i) {
        ArchonValue value = inst->args[i];
        if (!use(ctx, fn, v, value, position))
            return 0;
        if (fn->types[value] != sig.params[i])
            return an_error(ctx, ARCHON_BACKEND_INVALID, "call argument type does not match callee");
    }
    return inst->op != ARCHON_MIR_CALL_INDIRECT ||
        verify_indirect_signature(ctx, m, fn, v, inst->a, sig);
}

static int verify_uses(ANContext *ctx, VerifyModule *m, const ArchonMIRFunction *fn,
    const VerifyFunction *v)
{
    size_t i, j;
    for (i = 0; i < fn->block_count; ++i) {
        const ArchonMIRBlock *bb = &fn->blocks[i];
        const ArchonMIRTerm *term = &bb->term;
        unsigned end = (unsigned)bb->inst_count + 1u;
        ctx->block = i;
        ctx->instruction = SIZE_MAX;
        if (!an_continue(ctx))
            return 0;
        for (j = 0; j < bb->inst_count; ++j) {
            const ArchonMIRInst *inst = &bb->insts[j];
            unsigned shape = an_shape(inst->op), position = (unsigned)j + 1u;
            ctx->instruction = j;
            if (!an_continue(ctx) ||
                ((shape & HAS_A) && !use(ctx, fn, v, inst->a, position)) ||
                ((shape & HAS_B) && !use(ctx, fn, v, inst->b, position)) ||
                !an_legal(ctx, fn, inst))
                return 0;
            if (an_is_call(inst->op) && !verify_call(ctx, m, fn, v, inst, position))
                return 0;
            if (inst->op == ARCHON_MIR_GLOBAL_ADDR || inst->op == ARCHON_MIR_FUNC_ADDR) {
                const Symbol *symbol;
                if (!an_symbol(inst->symbol))
                    return an_error(ctx, ARCHON_BACKEND_INVALID, "invalid address symbol");
                symbol = find_symbol(m, inst->symbol);
                if (!symbol || ((symbol->kind == SYM_GLOBAL) != (inst->op == ARCHON_MIR_GLOBAL_ADDR)))
                    return an_error(ctx, ARCHON_BACKEND_INVALID, inst->op == ARCHON_MIR_GLOBAL_ADDR ?
                        "address target must be a declared global" :
                        "function address requires a declared function or import");
            }
        }
        ctx->instruction = SIZE_MAX;
        if (term->kind == ARCHON_MIR_RETURN) {
            if (fn->result == ARCHON_MIR_VOID) {
                if (term->value != ARCHON_MIR_NONE)
                    return an_error(ctx, ARCHON_BACKEND_INVALID, "void return has a value");
            } else {
                if (!use(ctx, fn, v, term->value, end))
                    return 0;
                if (fn->types[term->value] != fn->result)
                    return an_error(ctx, ARCHON_BACKEND_INVALID, "return type does not match function");
            }
        } else if (term->kind == ARCHON_MIR_CBR) {
            if (!use(ctx, fn, v, term->value, end))
                return 0;
            if (fn->types[term->value] != ARCHON_MIR_I64)
                return an_error(ctx, ARCHON_BACKEND_INVALID, "branch condition must be i64");
        }
        for (j = 0; j < edge_count(term); ++j) {
            const ArchonMIREdge *edge = edge_at(term, (unsigned)j);
            const ArchonMIRBlock *target = &fn->blocks[edge->target];
            size_t k;
            if (edge->arg_count != target->param_count)
                return an_error(ctx, ARCHON_BACKEND_INVALID, "edge argument count does not match block");
            for (k = 0; k < edge->arg_count; ++k) {
                ArchonValue arg = edge->args[k];
                if (!use(ctx, fn, v, arg, end))
                    return 0;
                if (fn->types[arg] != fn->types[target->params[k]])
                    return an_error(ctx, ARCHON_BACKEND_INVALID, "edge argument type does not match block");
            }
        }
    }
    ctx->block = SIZE_MAX;
    for (i = 0; i < fn->value_count; ++i)
        if (!v->defs[i].block)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "value table entry has no SSA definition");
    return 1;
}

static int verify_function(ANContext *ctx, VerifyModule *m, const ArchonMIRFunction *fn)
{
    VerifyFunction *v = calloc(1, sizeof(*v));
    int ok = 0;
    if (!v)
        return an_error(ctx, ARCHON_BACKEND_NOMEM, "allocating CFG verifier scratch");
    if (collect(ctx, m, fn, v)) {
        /* Declarations already bound block_count, so this matrix uses at most
         * ARCHON_MIR_MAX_BLOCKS rows of AN_DOM_WORDS words. */
        v->dom_words = (unsigned)((fn->block_count + 63u) / 64u);
        v->dom = calloc(fn->block_count * v->dom_words, sizeof(*v->dom));
        if (v->dom)
            ok = dominators(ctx, m, fn, v) && verify_uses(ctx, m, fn, v);
        else
            an_error(ctx, ARCHON_BACKEND_NOMEM, "allocating dominance sets");
    }
    free(v->dom);
    free(v);
    return ok;
}

int an_verify(ANContext *ctx, const ArchonMIRModule *module)
{
    VerifyModule m;
    size_t i;
    ctx->stage = ARCHON_BACKEND_VERIFY;
    ctx->function = ctx->block = ctx->instruction = SIZE_MAX;
    if (!an_continue(ctx))
        return 0;
    if (!module)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "missing MachineIR module");
    if (module->version != ARCHON_MIR_SCHEMA_VERSION)
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "unsupported MachineIR version");
    if (!array_ok(ctx, module->functions, module->function_count, AN_MAX_SYMBOLS, "invalid function table") ||
        !array_ok(ctx, module->imports, module->import_count, AN_MAX_SYMBOLS, "invalid import table") ||
        !array_ok(ctx, module->globals, module->global_count, AN_MAX_SYMBOLS, "invalid global table"))
        return 0;
    if (module->function_count + module->import_count + module->global_count > AN_MAX_SYMBOLS)
        return an_error(ctx, ARCHON_BACKEND_LIMIT, "module symbol limit");
    m.module = module;
    m.symbol_count = 0;
    m.work_left = AN_MAX_VERIFY_WORK;
    m.insts_left = AN_MAX_MODULE_INSTS;
    m.words_left = AN_MAX_GLOBAL_WORDS;
    if (!verify_declarations(ctx, &m))
        return 0;
    for (i = 0; i < module->function_count; ++i) {
        ctx->function = i;
        if (!verify_function(ctx, &m, &module->functions[i]))
            return 0;
    }
    ctx->function = SIZE_MAX;
    return an_continue(ctx);
}
