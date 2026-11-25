#include "NativeInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AN_GENERIC_MIR_VERIFY
typedef void (*ANVerifiedTextVisitor)(void *, const ArchonMIRModule *,
    const size_t *, const size_t *);
#endif

/* Parsing allocates only in this call. Payload types must align no more
 * strictly than the arena header; all payloads are C99 scalar or pointer
 * records (not compiler-specific over-aligned types). */
typedef union ParseAlloc ParseAlloc;
union ParseAlloc {
    ParseAlloc *next;
    uint64_t u64;
    long double ld;
    void *ptr;
};
typedef struct {
    const char *text;
    size_t size, pos, begin, length, allocated, instructions;
    ParseAlloc *allocs;
    ANContext ctx;
    ArchonBackendStatus status;
#ifdef AN_GENERIC_MIR_VERIFY
    size_t *starts, *ends, span;
#endif
} Parser;

static int fail(Parser *p, ArchonBackendStatus status, const char *reason)
{
    char message[160];
    if (p->status == ARCHON_BACKEND_OK) {
        p->status = status;
        (void)snprintf(message, sizeof(message), "MachineIR text byte %zu: %s",
            p->begin, reason);
        an_error(&p->ctx, status, message);
    }
    return 0;
}

static void *arena(Parser *p, size_t count, size_t item)
{
    ParseAlloc *node;
    const size_t limit = 32u * 1024u * 1024u;
    if (!count)
        return NULL;
    if (limit - p->allocated < sizeof(*node) ||
        count > (limit - p->allocated - sizeof(*node)) / item) {
        fail(p, ARCHON_BACKEND_LIMIT, "parser allocation budget exceeded");
        return NULL;
    }
    node = calloc(1, sizeof(*node) + count * item);
    if (!node) {
        fail(p, ARCHON_BACKEND_NOMEM, "unable to allocate parse records");
        return NULL;
    }
    p->allocated += sizeof(*node) + count * item;
    node->next = p->allocs;
    p->allocs = node;
    return node + 1;
}

static int space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void token(Parser *p)
{
    while (p->pos < p->size && space(p->text[p->pos]))
        ++p->pos;
    p->begin = p->pos;
    while (p->pos < p->size && !space(p->text[p->pos]))
        ++p->pos;
    p->length = p->pos - p->begin;
}

static int is_token(const Parser *p, const char *word)
{
    return strlen(word) == p->length &&
        memcmp(p->text + p->begin, word, p->length) == 0;
}

static int keyword(Parser *p, const char *word)
{
    token(p);
    return is_token(p, word) || fail(p, ARCHON_BACKEND_INVALID, word);
}

/* Unsigned decimal, or unsigned 0x hex where a word is expected. A negative
 * architectural word is written as its two's-complement bit pattern. */
static int number(Parser *p, uint64_t *result, int hex_allowed)
{
    uint64_t value = 0, radix = 10;
    size_t i = 0;
    token(p);
    if (hex_allowed && p->length >= 2 && p->text[p->begin] == '0' &&
        p->text[p->begin + 1] == 'x') {
        radix = 16;
        i = 2;
    }
    if (i == p->length)
        return fail(p, ARCHON_BACKEND_INVALID, "expected integer");
    for (; i < p->length; ++i) {
        char c = p->text[p->begin + i];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (unsigned)(c - 'A' + 10);
        else
            return fail(p, ARCHON_BACKEND_INVALID, "invalid integer digit");
        if (d >= radix || value > (UINT64_MAX - d) / radix)
            return fail(p, ARCHON_BACKEND_INVALID, "integer out of range");
        value = value * radix + d;
    }
    *result = value;
    return 1;
}

static int count(Parser *p, size_t limit, size_t *result)
{
    uint64_t n;
    if (!number(p, &n, 0))
        return 0;
    if (n > limit)
        return fail(p, ARCHON_BACKEND_LIMIT, "count or index exceeds profile limit");
    *result = (size_t)n;
    return 1;
}

static int value(Parser *p, ArchonValue *result, int optional)
{
    size_t saved = p->pos, n;
    if (optional) {
        token(p);
        if (is_token(p, "-")) {
            *result = ARCHON_MIR_NONE;
            return 1;
        }
        p->pos = saved;
    }
    if (!count(p, AN_MAX_VALUE - 1u, &n))
        return 0;
    *result = (ArchonValue)n;
    return 1;
}

static int values(Parser *p, size_t n, const ArchonValue **result)
{
    ArchonValue *data = arena(p, n, sizeof(*data));
    size_t i;
    if (n && !data)
        return 0;
    for (i = 0; i < n; ++i)
        if (!value(p, &data[i], 0))
            return 0;
    *result = data;
    return 1;
}

static int type(Parser *p, ArchonMIRType *out)
{
    token(p);
    if (is_token(p, "i64"))
        *out = ARCHON_MIR_I64;
    else if (is_token(p, "ptr"))
        *out = ARCHON_MIR_PTR;
    else if (is_token(p, "func_ptr"))
        *out = ARCHON_MIR_FUNC_PTR;
    else if (is_token(p, "f32"))
        *out = ARCHON_MIR_F32;
    else if (is_token(p, "f64"))
        *out = ARCHON_MIR_F64;
    else if (is_token(p, "f128"))
        *out = ARCHON_MIR_F128;
    else if (is_token(p, "f80"))
        *out = ARCHON_MIR_F80;
    else if (is_token(p, "void"))
        *out = ARCHON_MIR_VOID;
    else
        return fail(p, ARCHON_BACKEND_UNSUPPORTED, "unknown type");
    return 1;
}

static int types(Parser *p, size_t n, const ArchonMIRType **out)
{
    ArchonMIRType *data = arena(p, n, sizeof(*data));
    size_t i;
    if (n && !data)
        return 0;
    for (i = 0; i < n; ++i)
        if (!type(p, &data[i]))
            return 0;
    *out = data;
    return 1;
}

static int name(Parser *p, const char **out)
{
    char *data;
    token(p);
    if (!p->length || p->length > 255 ||
        memchr(p->text + p->begin, '\0', p->length))
        return fail(p, ARCHON_BACKEND_INVALID, "invalid symbol length or NUL");
    data = arena(p, p->length + 1, 1);
    if (!data)
        return 0;
    memcpy(data, p->text + p->begin, p->length);
    if (!an_symbol(data))
        return fail(p, ARCHON_BACKEND_INVALID, "invalid symbol");
    *out = data;
    return 1;
}

static int inst(Parser *p, ArchonMIRInst *ins)
{
    static const struct { const char *name; ArchonMIROpcode op; } ops[] = {
        {"const", ARCHON_MIR_CONST}, {"copy", ARCHON_MIR_COPY},
        {"add", ARCHON_MIR_ADD}, {"sub", ARCHON_MIR_SUB}, {"mul", ARCHON_MIR_MUL},
        {"div_s", ARCHON_MIR_DIV_S}, {"rem_s", ARCHON_MIR_REM_S},
        {"div_u", ARCHON_MIR_DIV_U}, {"rem_u", ARCHON_MIR_REM_U},
        {"and", ARCHON_MIR_AND}, {"or", ARCHON_MIR_OR}, {"xor", ARCHON_MIR_XOR},
        {"shl", ARCHON_MIR_SHL}, {"lshr", ARCHON_MIR_LSHR}, {"ashr", ARCHON_MIR_ASHR},
        {"eq", ARCHON_MIR_EQ}, {"slt", ARCHON_MIR_SLT}, {"ult", ARCHON_MIR_ULT},
        {"add_checked_s", ARCHON_MIR_ADD_CHECKED_S},
        {"sub_checked_s", ARCHON_MIR_SUB_CHECKED_S},
        {"div_checked_s", ARCHON_MIR_DIV_CHECKED_S},
        {"mul_checked_s", ARCHON_MIR_MUL_CHECKED_S},
        {"trunc", ARCHON_MIR_TRUNC}, {"zext", ARCHON_MIR_ZEXT}, {"sext", ARCHON_MIR_SEXT},
        {"ptr_add", ARCHON_MIR_PTR_ADD}, {"load", ARCHON_MIR_LOAD},
        {"load_s8", ARCHON_MIR_LOAD_S8}, {"load_u8", ARCHON_MIR_LOAD_U8},
        {"load_s16", ARCHON_MIR_LOAD_S16}, {"load_u16", ARCHON_MIR_LOAD_U16},
        {"load_s32", ARCHON_MIR_LOAD_S32}, {"load_u32", ARCHON_MIR_LOAD_U32},
        {"load_ptr", ARCHON_MIR_LOAD_PTR}, {"load_func_ptr", ARCHON_MIR_LOAD_FUNC_PTR},
        {"store", ARCHON_MIR_STORE}, {"frame_addr", ARCHON_MIR_FRAME_ADDR},
        {"store8", ARCHON_MIR_STORE8}, {"store16", ARCHON_MIR_STORE16},
        {"store32", ARCHON_MIR_STORE32}, {"store_ptr", ARCHON_MIR_STORE_PTR},
        {"store_func_ptr", ARCHON_MIR_STORE_FUNC_PTR},
        {"ptr_to_int", ARCHON_MIR_PTR_TO_INT}, {"int_to_ptr", ARCHON_MIR_INT_TO_PTR},
        {"func_ptr_to_int", ARCHON_MIR_FUNC_PTR_TO_INT},
        {"int_to_func_ptr", ARCHON_MIR_INT_TO_FUNC_PTR},
        {"global_addr", ARCHON_MIR_GLOBAL_ADDR}, {"call", ARCHON_MIR_CALL},
        {"func_addr", ARCHON_MIR_FUNC_ADDR}, {"call_indirect", ARCHON_MIR_CALL_INDIRECT},
        {"fconst", ARCHON_MIR_FCONST}, {"fload", ARCHON_MIR_FLOAD}, {"fstore", ARCHON_MIR_FSTORE},
        {"fneg", ARCHON_MIR_FNEG}, {"fadd", ARCHON_MIR_FADD}, {"fsub", ARCHON_MIR_FSUB},
        {"fmul", ARCHON_MIR_FMUL}, {"fdiv", ARCHON_MIR_FDIV}, {"feq", ARCHON_MIR_FEQ},
        {"flt", ARCHON_MIR_FLT}, {"fle", ARCHON_MIR_FLE}, {"fcast", ARCHON_MIR_FCAST},
        {"sitofp", ARCHON_MIR_SITOFP}, {"uitofp", ARCHON_MIR_UITOFP},
        {"fptosi", ARCHON_MIR_FPTOSI}, {"fptoui", ARCHON_MIR_FPTOUI},
        {"amd64_va_start", ARCHON_MIR_AMD64_VA_START},
        {"aarch64_va_start", ARCHON_MIR_AARCH64_VA_START},
        {"rv64_va_start", ARCHON_MIR_RV64_VA_START},
        {"amd64_x87_add", ARCHON_MIR_AMD64_X87_ADD},
        {"amd64_x87_sub", ARCHON_MIR_AMD64_X87_SUB},
        {"amd64_x87_mul", ARCHON_MIR_AMD64_X87_MUL},
        {"amd64_x87_div", ARCHON_MIR_AMD64_X87_DIV},
        {"amd64_x87_eq", ARCHON_MIR_AMD64_X87_EQ},
        {"amd64_x87_lt", ARCHON_MIR_AMD64_X87_LT},
        {"amd64_x87_le", ARCHON_MIR_AMD64_X87_LE},
        {"amd64_x87_from_fp", ARCHON_MIR_AMD64_X87_FROM_FP},
        {"f128_pack", ARCHON_MIR_F128_PACK},
        {"f128_low", ARCHON_MIR_F128_LOW},
        {"f128_high", ARCHON_MIR_F128_HIGH},
        {"stack_alloc", ARCHON_MIR_STACK_ALLOC}, {"stack_free", ARCHON_MIR_STACK_FREE},
        {"amd64_x87_pack", ARCHON_MIR_AMD64_X87_PACK},
        {"amd64_x87_low", ARCHON_MIR_AMD64_X87_LOW},
        {"amd64_x87_high", ARCHON_MIR_AMD64_X87_HIGH},
        {"amd64_x87_to_fp", ARCHON_MIR_AMD64_X87_TO_FP},
        {"amd64_x87_from_si", ARCHON_MIR_AMD64_X87_FROM_SI},
        {"amd64_x87_from_ui", ARCHON_MIR_AMD64_X87_FROM_UI},
        {"amd64_x87_to_si", ARCHON_MIR_AMD64_X87_TO_SI},
        {"amd64_x87_to_ui", ARCHON_MIR_AMD64_X87_TO_UI}
    };
    size_t i;
    unsigned shape;
    token(p);
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
        if (is_token(p, ops[i].name)) {
            ins->op = ops[i].op;
            break;
        }
    if (!ins->op)
        return fail(p, ARCHON_BACKEND_UNSUPPORTED, "unknown operation");
    shape = an_shape(ins->op);
    ins->dst = ins->a = ins->b = ARCHON_MIR_NONE;
    if (((shape & 1u) && !value(p, &ins->dst, an_is_call(ins->op))) ||
        ((shape & 2u) && !value(p, &ins->a, 0)) ||
        ((shape & 4u) && !value(p, &ins->b, 0)))
        return 0;
    switch (ins->op) {
    case ARCHON_MIR_CONST:
    case ARCHON_MIR_FCONST:
    case ARCHON_MIR_FLOAD:
    case ARCHON_MIR_FSTORE:
    case ARCHON_MIR_LOAD:
    case ARCHON_MIR_LOAD_S8:
    case ARCHON_MIR_LOAD_U8:
    case ARCHON_MIR_LOAD_S16:
    case ARCHON_MIR_LOAD_U16:
    case ARCHON_MIR_LOAD_S32:
    case ARCHON_MIR_LOAD_U32:
    case ARCHON_MIR_LOAD_PTR:
    case ARCHON_MIR_LOAD_FUNC_PTR:
    case ARCHON_MIR_STORE:
    case ARCHON_MIR_STORE8:
    case ARCHON_MIR_STORE16:
    case ARCHON_MIR_STORE32:
    case ARCHON_MIR_STORE_PTR:
    case ARCHON_MIR_STORE_FUNC_PTR:
    case ARCHON_MIR_FRAME_ADDR:
    case ARCHON_MIR_TRUNC:
    case ARCHON_MIR_ZEXT:
    case ARCHON_MIR_SEXT:
        return number(p, &ins->imm, 1);
    case ARCHON_MIR_GLOBAL_ADDR:
    case ARCHON_MIR_FUNC_ADDR:
        return name(p, &ins->symbol);
    case ARCHON_MIR_CALL:
    case ARCHON_MIR_CALL_INDIRECT:
        return name(p, &ins->symbol) &&
            count(p, AN_MAX_PARAMS, &ins->arg_count) &&
            values(p, ins->arg_count, &ins->args);
    default:
        return 1;
    }
}

static int edge(Parser *p, ArchonMIREdge *e)
{
    size_t target;
    if (!count(p, AN_MAX_BLOCK - 1u, &target))
        return 0;
    e->target = (uint32_t)target;
    return count(p, AN_MAX_VALUE, &e->arg_count) && values(p, e->arg_count, &e->args);
}

static int term(Parser *p, ArchonMIRTerm *t)
{
    t->value = ARCHON_MIR_NONE;
    token(p);
    if (is_token(p, "ret")) {
        t->kind = ARCHON_MIR_RETURN;
        return value(p, &t->value, 1);
    }
    if (is_token(p, "br")) {
        t->kind = ARCHON_MIR_BR;
        return edge(p, &t->yes);
    }
    if (is_token(p, "cbr")) {
        t->kind = ARCHON_MIR_CBR;
        return value(p, &t->value, 0) && edge(p, &t->yes) && edge(p, &t->no);
    }
    if (is_token(p, "trap")) {
        t->kind = ARCHON_MIR_TRAP;
        return 1;
    }
    return fail(p, ARCHON_BACKEND_INVALID, "expected a terminator");
}

static int poll_parser(Parser *p)
{
    if (p->ctx.options && p->ctx.options->cancelled &&
        p->ctx.options->cancelled(p->ctx.options->user))
        return fail(p, ARCHON_BACKEND_CANCELLED, "compilation cancelled");
    return 1;
}

static int function(Parser *p, ArchonMIRFunction *fn)
{
    ArchonMIRBlock *blocks;
    ArchonMIRLocal *locals;
    size_t b, i, total = 0;
    if (!keyword(p, "function") || !name(p, &fn->name) || !type(p, &fn->result) ||
        !count(p, AN_MAX_PARAMS, &fn->param_count) ||
        !count(p, AN_MAX_VALUE, &fn->value_count) ||
        !count(p, AN_MAX_BLOCK, &fn->block_count) ||
        !count(p, AN_MAX_VALUE, &fn->local_count) ||
        !keyword(p, "types") || !types(p, fn->value_count, &fn->types))
        return 0;
    locals = arena(p, fn->local_count, sizeof(*locals));
    blocks = arena(p, fn->block_count, sizeof(*blocks));
    if ((fn->local_count && !locals) || (fn->block_count && !blocks))
        return 0;
    fn->locals = locals;
    fn->blocks = blocks;
    for (i = 0; i < fn->local_count; ++i) {
        uint64_t size, align;
        if (!keyword(p, "local") || !number(p, &size, 0) || !number(p, &align, 0))
            return 0;
        if (size > UINT32_MAX || align > UINT32_MAX)
            return fail(p, ARCHON_BACKEND_INVALID, "local size or alignment overflows");
        locals[i].size = (uint32_t)size;
        locals[i].alignment = (uint32_t)align;
    }
    for (b = 0; b < fn->block_count; ++b) {
        ArchonMIRBlock *blk = &blocks[b];
        ArchonMIRInst *instructions;
        p->ctx.block = b;
        if (!poll_parser(p) || !keyword(p, "block") ||
            !count(p, AN_MAX_VALUE, &blk->param_count) ||
            !count(p, AN_MAX_INSTS - total, &blk->inst_count) ||
            !keyword(p, "params") || !values(p, blk->param_count, &blk->params))
            return 0;
        total += blk->inst_count;
        if (blk->inst_count > AN_MAX_MODULE_INSTS - p->instructions)
            return fail(p, ARCHON_BACKEND_LIMIT, "module instruction limit exceeded");
        p->instructions += blk->inst_count;
        instructions = arena(p, blk->inst_count, sizeof(*instructions));
        if (blk->inst_count && !instructions)
            return 0;
        blk->insts = instructions;
        for (i = 0; i < blk->inst_count; ++i) {
            p->ctx.instruction = i;
            if (!(i % 128u) && !poll_parser(p))
                return 0;
#ifdef AN_GENERIC_MIR_VERIFY
            if (p->starts) {
                while (p->pos < p->size && space(p->text[p->pos]))
                    ++p->pos;
                p->starts[p->span] = p->pos;
            }
#endif
            if (!inst(p, &instructions[i]))
                return 0;
#ifdef AN_GENERIC_MIR_VERIFY
            if (p->starts)
                p->ends[p->span++] = p->pos;
#endif
        }
        p->ctx.instruction = blk->inst_count;
        if (!term(p, &blk->term))
            return 0;
        p->ctx.instruction = SIZE_MAX;
    }
    p->ctx.block = SIZE_MAX;
    return 1;
}

static int module(Parser *p, ArchonMIRModule *out)
{
    ArchonMIRFunction *functions;
    ArchonMIRImport *imports;
    ArchonMIRGlobal *globals;
    size_t i, j;
    uint64_t version;
    if (!keyword(p, "archon-mir") || !number(p, &version, 0))
        return 0;
    if (version != ARCHON_MIR_SCHEMA_VERSION)
        return fail(p, ARCHON_BACKEND_UNSUPPORTED, "unsupported schema version");
    out->version = (unsigned)version;
    if (!keyword(p, "module") || !count(p, AN_MAX_SYMBOLS, &out->function_count) ||
        !count(p, AN_MAX_SYMBOLS, &out->import_count) ||
        !count(p, AN_MAX_SYMBOLS, &out->global_count))
        return 0;
    functions = arena(p, out->function_count, sizeof(*functions));
    imports = arena(p, out->import_count, sizeof(*imports));
    globals = arena(p, out->global_count, sizeof(*globals));
    if ((out->function_count && !functions) || (out->import_count && !imports) ||
        (out->global_count && !globals))
        return 0;
    out->functions = functions;
    out->imports = imports;
    out->globals = globals;
    for (i = 0; i < out->function_count; ++i) {
        p->ctx.function = i;
        if (!poll_parser(p) || !function(p, &functions[i]))
            return 0;
    }
    p->ctx.function = SIZE_MAX;
    for (i = 0; i < out->import_count; ++i) {
        ArchonMIRImport *im = &imports[i];
        if (!poll_parser(p) || !keyword(p, "import") || !name(p, &im->name) ||
            !type(p, &im->result) || !count(p, AN_MAX_PARAMS, &im->param_count) ||
            !types(p, im->param_count, &im->params))
            return 0;
    }
    for (i = 0; i < out->global_count; ++i) {
        ArchonMIRGlobal *g = &globals[i];
        uint64_t *words;
        if (!poll_parser(p) || !keyword(p, "global") || !name(p, &g->name) ||
            !count(p, ARCHON_MIR_TEXT_MAX_BYTES / 8u, &g->word_count))
            return 0;
        token(p);
        if (is_token(p, "zero"))
            continue;
        if (!g->word_count || !is_token(p, "init"))
            return fail(p, ARCHON_BACKEND_INVALID, "expected zero or nonempty init");
        words = arena(p, g->word_count, sizeof(*words));
        if (!words)
            return 0;
        g->words = words;
        for (j = 0; j < g->word_count; ++j) {
            if (!(j % 128u) && !poll_parser(p))
                return 0;
            if (!number(p, &words[j], 1))
                return 0;
        }
    }
    if (!keyword(p, "end"))
        return 0;
    token(p);
    return !p->length || fail(p, ARCHON_BACKEND_INVALID, "trailing input");
}

static ArchonBackendStatus compile_text(const char *text, size_t size,
    const ArchonBackendOptions *options, unsigned flags, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic, const ArchonBackendScheduleHints *hints,
    const ArchonBackendFactHints *facts
#ifdef AN_GENERIC_MIR_VERIFY
    , ANVerifiedTextVisitor visitor, void *user
#endif
    )
{
    Parser p = {0};
    ArchonMIRModule m = {0};
    ArchonBackendDiagnostic parse_diag = {
        .function = SIZE_MAX, .block = SIZE_MAX, .instruction = SIZE_MAX
    };
    ArchonBackendStatus status;
    p.ctx.diag = &parse_diag;
    p.ctx.stage = ARCHON_BACKEND_INPUT;
    p.ctx.function = p.ctx.block = p.ctx.instruction = SIZE_MAX;
    p.ctx.options = options;
    p.text = text;
    p.size = size;
    if (!text || !artifact || artifact->data || artifact->size)
        fail(&p, ARCHON_BACKEND_INVALID, "require text and an empty output artifact");
    else if (size > ARCHON_MIR_TEXT_MAX_BYTES)
        fail(&p, ARCHON_BACKEND_LIMIT, "input byte limit exceeded");
#ifdef AN_GENERIC_MIR_VERIFY
    (void)hints;
    (void)facts;
    if (p.status == ARCHON_BACKEND_OK && visitor) {
        /* Every recorded instruction occupies at least one input byte. */
        const size_t spans = size < AN_MAX_MODULE_INSTS ? size + 1 : AN_MAX_MODULE_INSTS;
        p.starts = arena(&p, spans, sizeof(*p.starts));
        p.ends = arena(&p, spans, sizeof(*p.ends));
    }
#endif
    status = p.status;
    if (status == ARCHON_BACKEND_OK) {
        if (poll_parser(&p) && module(&p, &m)) {
#ifdef AN_GENERIC_MIR_VERIFY
            status = archon_backend_compile_ex(&m, options, flags, artifact, &parse_diag);
            if (status == ARCHON_BACKEND_OK && visitor)
                visitor(user, &m, p.starts, p.ends);
#else
            if (hints || facts)
                status = an_compile_with_hints(&m, options, flags, hints, facts, artifact, &parse_diag);
            else
                status = archon_backend_compile_ex(&m, options, flags, artifact, &parse_diag);
#endif
        } else
            status = p.status;
    }
    while (p.allocs) {
        ParseAlloc *next = p.allocs->next;
        free(p.allocs);
        p.allocs = next;
    }
    if (diagnostic)
        *diagnostic = parse_diag;
    return status;
}

#ifdef AN_GENERIC_MIR_VERIFY
ArchonBackendStatus cephyr_visit_mir(const char *text, size_t size, ANVerifiedTextVisitor visitor,
    void *user, ArchonBackendDiagnostic *diagnostic)
{
    ArchonBackendArtifact unused = {0};
    return compile_text(text, size, NULL, 0, &unused, diagnostic, NULL, NULL, visitor, user);
}
#else
ArchonBackendStatus archon_backend_compile_text_with_schedule_hints(const char *text, size_t size,
    const ArchonBackendOptions *options, unsigned flags, const ArchonBackendScheduleHints *hints,
    ArchonBackendArtifact *artifact, ArchonBackendDiagnostic *diagnostic)
{
    return compile_text(text, size, options, flags, artifact, diagnostic, hints, NULL);
}

ArchonBackendStatus archon_backend_compile_text_with_facts(const char *text, size_t size,
    const ArchonBackendOptions *options, unsigned flags, const ArchonBackendFactHints *facts,
    ArchonBackendArtifact *artifact, ArchonBackendDiagnostic *diagnostic)
{
    return compile_text(text, size, options, flags, artifact, diagnostic, NULL, facts);
}
#endif

ArchonBackendStatus archon_backend_compile_text_ex(const char *text, size_t size,
    const ArchonBackendOptions *options, unsigned flags, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic)
{
    return compile_text(text, size, options, flags, artifact, diagnostic, NULL, NULL
#ifdef AN_GENERIC_MIR_VERIFY
        , NULL, NULL
#endif
        );
}

ArchonBackendStatus archon_backend_compile_text(const char *text, size_t size,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic)
{
    return archon_backend_compile_text_ex(text, size, options, 0, artifact, diagnostic);
}
