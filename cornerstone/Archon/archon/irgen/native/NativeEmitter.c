#include "NativeInternal.h"
#include "TargetNative.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* Capacity includes the NUL; the public byte limit and size do not. */
static int reserve(ANOut *out, size_t extra)
{
    size_t need, capacity, ceiling;
    char *data;

    if (out->size > out->limit || extra > out->limit - out->size ||
        extra >= SIZE_MAX - out->size) {
        out->status = ARCHON_BACKEND_LIMIT;
        return 0;
    }
    need = out->size + extra + 1;
    if (need <= out->capacity)
        return 1;

    ceiling = out->limit == SIZE_MAX ? SIZE_MAX : out->limit + 1;
    capacity = out->capacity;
    if (!capacity)
        capacity = ceiling < 256 ? ceiling : 256;
    while (capacity < need)
        capacity = capacity > ceiling / 2 ? ceiling : capacity * 2;

    data = realloc(out->data, capacity);
    if (!data) {
        out->status = ARCHON_BACKEND_NOMEM;
        return 0;
    }
    out->data = data;
    out->capacity = capacity;
    return 1;
}

static void append(ANOut *out, size_t instructions, const char *format, ...)
{
    va_list args;
    int count, written;

    if (out->status != ARCHON_BACKEND_OK)
        return;
    if (instructions > SIZE_MAX - out->instructions) {
        out->status = ARCHON_BACKEND_LIMIT;
        return;
    }

    va_start(args, format);
    count = vsnprintf(NULL, 0, format, args);
    va_end(args);
    if (count < 0) {
        out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
    if (!reserve(out, (size_t)count))
        return;

    va_start(args, format);
    written = vsnprintf(out->data + out->size, (size_t)count + 1, format, args);
    va_end(args);
    if (written != count) {
        out->data[out->size] = '\0';
        out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
    out->size += (size_t)count;
    out->instructions += instructions;
}

static void emit_words(ANOut *out, const uint32_t *words, size_t count)
{
    size_t i;
    if (out->status != ARCHON_BACKEND_OK)
        return;
    if (count > SIZE_MAX / 4 || count > SIZE_MAX - out->instructions) {
        an_out_error(out, ARCHON_BACKEND_LIMIT, "native instruction count exceeds output limit");
        return;
    }
    if (!reserve(out, count * 4))
        return;
    for (i = 0; i < count; ++i)
        an_store_word(out->data + out->size + 4 * i, words[i]);
    out->size += 4 * count;
    out->instructions += count;
    out->data[out->size] = '\0';
}

static int valid_symbol(ANOut *out, const char *name)
{
    if (out->status != ARCHON_BACKEND_OK)
        return 0;
    if (!an_symbol(name)) {
        out->status = ARCHON_BACKEND_INVALID;
        return 0;
    }
    return 1;
}

/* Physical v1 serializes the same validated operands as GAS, before symbolic
 * transfers expand. Each LF-terminated record has space-separated fields.
 * xN is a hardware register and instruction immediates are signed decimal.
 * FN:LABEL identifies a label by function layout index and local label index.
 * LD, SD and JALR place the base register before the displacement. The
 * instruction counter tracks GAS expansion, not the number of records. */
void an_emit(ANOut *out, ANOpcode op, unsigned rd, unsigned rs1,
    unsigned rs2, int imm)
{
    uint32_t word;
    const char *name;

    if (out->status != ARCHON_BACKEND_OK)
        return;
    if (!an_encode(op, rd, rs1, rs2, imm, &word)) {
        an_out_error(out, ARCHON_BACKEND_INTERNAL, "physical operand cannot be encoded");
        return;
    }
    if (out->machine_code) {
        emit_words(out, &word, 1);
        return;
    }
    name = an_encodings[op].mnemonic;
    switch (op) {
    case AN_ADD: case AN_SUB: case AN_MUL: case AN_DIV: case AN_REM:
    case AN_DIVU: case AN_REMU: case AN_AND: case AN_OR: case AN_XOR:
    case AN_SLL: case AN_SRL: case AN_SRA: case AN_SLT: case AN_SLTU:
        append(out, 1, out->physical ? "inst %s x%u x%u x%u\n" : "\t%s x%u, x%u, x%u\n",
            name, rd, rs1, rs2);
        return;
    case AN_ADDI:
    case AN_SLLI:
        append(out, 1, out->physical ? "inst %s x%u x%u %d\n" : "\t%s x%u, x%u, %d\n",
            name, rd, rs1, imm);
        return;
    case AN_LD:
    case AN_SD:
    case AN_JALR:
        if (out->physical)
            append(out, 1, "inst %s x%u x%u %d\n", name, rd, rs1, imm);
        else
            append(out, 1, "\t%s x%u, %d(x%u)\n", name, rd, imm, rs1);
        return;
    case AN_EBREAK:
        append(out, 1, out->physical ? "inst %s\n" : "\t%s\n", name);
        return;
    default:
        an_out_error(out, ARCHON_BACKEND_INTERNAL, "invalid direct physical instruction form");
        return;
    }
}

void an_label(ANOut *out, size_t fn, unsigned label)
{
    if (an_layout_label(out, fn, label) && !out->machine_code)
        append(out, 0, out->physical ? "label %zu:%u\n" : ".Larchon%zu_%u:\n", fn, label);
}

/* The low relocation refers to the anchor on AUIPC, not to the destination.
 * The pair is emitted together, so 2b always finds its own anchor. */
#define AN_JUMP_FORMAT \
    "2:\n\t" AN_MN_AUIPC " x%u, %%pcrel_hi(.Larchon%zu_%u)\n" \
    "\t" AN_MN_JALR " x0, %%pcrel_lo(2b)(x%u)\n"

void an_jump(ANOut *out, size_t fn, unsigned label)
{
    if (!an_layout_reference(out, fn, label, 0))
        return;
    if (out->machine_code) {
        const uint32_t pending[2] = {0, 0};
        emit_words(out, pending, 2);
    } else if (out->physical)
        append(out, 2, "jump x%u %zu:%u\n", AN_T3, fn, label);
    else
        append(out, 2, AN_JUMP_FORMAT, AN_T3, fn, label, AN_T3);
}

void an_branch(ANOut *out, int not_equal, unsigned a, unsigned b,
    size_t fn, unsigned label)
{
    uint32_t words[3] = {0, 0, 0};
    if (out->status != ARCHON_BACKEND_OK)
        return;
    /* The inverted branch skips itself and two uncompressed instructions. */
    if ((not_equal != 0 && not_equal != 1) ||
        !an_encode(not_equal ? AN_BEQ : AN_BNE, a, b, 0, 12, &words[0])) {
        an_out_error(out, ARCHON_BACKEND_INTERNAL, "invalid native conditional transfer");
        return;
    }
    if (!an_layout_reference(out, fn, label, 1))
        return;
    if (out->machine_code) {
        emit_words(out, words, 3);
        return;
    }
    /* The record states the condition to take the target and the scratch
     * register used on that path, not the inverted GAS skip condition. */
    if (out->physical) {
        append(out, 3, "branch %s x%u x%u x%u %zu:%u\n",
            not_equal ? AN_MN_BNE : AN_MN_BEQ, a, b, AN_T3, fn, label);
        return;
    }
    append(out, 3, "\t%s x%u, x%u, 1f\n" AN_JUMP_FORMAT "1:\n",
        not_equal ? AN_MN_BEQ : AN_MN_BNE, a, b, AN_T3, fn, label, AN_T3);
}

static void emit_local_symbol(ANOut *out, const char *name, unsigned dst, ANOpcode lower)
{
    size_t to;
    uint32_t words[2];
    if (!an_layout_entry(out, name, &to))
        return;
    if (!an_encode_pcrel(out->instructions, to, dst, dst, lower, words)) {
        an_out_error(out, ARCHON_BACKEND_LIMIT, "native local symbol is out of range");
        return;
    }
    emit_words(out, words, 2);
}

void an_call_symbol(ANOut *out, const char *name)
{
    if (!valid_symbol(out, name))
        return;
    if (out->machine_code)
        emit_local_symbol(out, name, an_abi.return_address, AN_JALR);
    else if (out->physical)
        append(out, 2, "call-symbol x%u %s\n", AN_RA, name);
    else
        append(out, 2, "\tcall %s\n", name);
}

void an_address_symbol(ANOut *out, unsigned dst, const char *name)
{
    if (!valid_symbol(out, name))
        return;
    if (!dst || dst > AN_T6) {
        an_out_error(out, ARCHON_BACKEND_INTERNAL, "invalid symbol destination register");
        return;
    }
    if (out->machine_code) {
        emit_local_symbol(out, name, dst, AN_ADDI);
        return;
    }
    append(out, 2, out->physical ? "address-symbol x%u %s\n" : "\tlla x%u, %s\n", dst, name);
}

void an_asm_header(ANOut *out)
{
    if (!out->machine_code)
        append(out, 0, out->physical ? "archon-physical 1\ntarget " AN_TARGET_NAME
            "\narchdb-sha256 " AN_TARGET_SHA256 "\n" :
            "\t.option push\n\t.option norvc\n\t.option norelax\n\t.text\n");
}

void an_asm_function(ANOut *out, const char *name)
{
    if (!valid_symbol(out, name) || out->machine_code)
        return;
    if (out->physical)
        append(out, 0, "function %s\n", name);
    else
        append(out, 0, "\t.p2align 2\n\t.globl %s\n\t.type %s, @function\n%s:\n",
            name, name, name);
}

void an_asm_end_function(ANOut *out, const char *name)
{
    if (!valid_symbol(out, name) || out->machine_code)
        return;
    if (out->physical)
        append(out, 0, "end-function %s\n", name);
    else
        append(out, 0, "\t.size %s, .-%s\n", name, name);
}

int an_asm_globals(ANContext *ctx, ANOut *out, const ArchonMIRModule *module)
{
    size_t i, j;

    for (i = 0; i < module->global_count && out->status == ARCHON_BACKEND_OK; ++i) {
        const ArchonMIRGlobal *global = &module->globals[i];
        const char *name = global->name;

        if (!an_continue(ctx))
            return 0;
        if (!valid_symbol(out, name))
            break;
        if (!global->word_count) {
            append(out, 0, out->physical ? "global %s extern\n" : "\t.extern %s\n", name);
            continue;
        }
        if (global->word_count > SIZE_MAX / 8) {
            out->status = ARCHON_BACKEND_LIMIT;
            break;
        }
        if (out->physical)
            append(out, 0, "global %s %s %zu\n", name,
                global->words ? "init" : "zero", global->word_count);
        else
            append(out, 0, "\t%s\n\t.p2align 3\n\t.globl %s\n\t.type %s, @object\n%s:\n",
                global->words ? ".data" : ".bss", name, name, name);
        if (global->words) {
            for (j = 0; j < global->word_count && out->status == ARCHON_BACKEND_OK; ++j) {
                if (!(j % 128u) && !an_continue(ctx))
                    return 0;
                append(out, 0, out->physical ? "word 0x%016" PRIx64 "\n" :
                    "\t.quad 0x%016" PRIx64 "\n", global->words[j]);
            }
        } else if (!out->physical) {
            append(out, 0, "\t.zero %zu\n", global->word_count * 8);
        }
        if (!out->physical)
            append(out, 0, "\t.size %s, .-%s\n", name, name);
    }
    return 1; /* Sink failures are diagnosed by the module driver. */
}

void an_asm_footer(ANOut *out)
{
    if (!out->machine_code)
        append(out, 0, out->physical ? "end\n" :
            "\t.section .note.GNU-stack,\"\",@progbits\n\t.option pop\n");
}
