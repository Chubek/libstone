/* Sandstone assembler directives: sections, symbols, data, alignment.
 *
 * Supported directives:
 *   sections  .text .data .rodata .bss | .section NAME
 *   symbols   .globl/.global NAME... | .extern NAME... |
 *             .equ/.set NAME, EXPR
 *   data      .byte/.short/.word/.long/.quad EXPR... |
 *             .ascii/.asciz/.string "..." |
 *             .zero/.space/.skip N[,FILL] | .fill COUNT,SIZE,VALUE |
 *             .align/.balign N[,FILL] | .p2align N[,FILL] | .org OFS[,FILL]
 *   ignored   .type .size (accepted for GAS compatibility)
 *
 * Expressions are integers, defined symbols, parentheses and + - * / %.
 * `.equ` lines are evaluated to a fixpoint (up to 8 rounds) so that
 * constants may refer to symbols defined later in the file.
 */
#include "intern.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ---- expression evaluation ------------------------------------------ */

typedef struct {
    sandstone_asm_t *ctx;
    const char *p;
    int ok;
} expr_cursor_t;

static int64_t expr_logic(expr_cursor_t *c);

static void expr_ws(expr_cursor_t *c) {
    while (isspace((unsigned char)*c->p))
        c->p++;
}

static int64_t expr_primary(expr_cursor_t *c) {
    expr_ws(c);
    if (*c->p == '(') {
        c->p++;
        int64_t v = expr_logic(c);
        expr_ws(c);
        if (*c->p != ')') {
            c->ok = 0;
            return 0;
        }
        c->p++;
        return v;
    }
    if (*c->p == '-' || *c->p == '+' || *c->p == '~' || *c->p == '!') {
        char op = *c->p++;
        int64_t v = expr_primary(c);
        if (op == '-') {
            if (v == INT64_MIN) c->ok = 0;
            else v = -v;
        }
        else if (op == '~')
            v = ~v;
        else if (op == '!')
            v = !v;
        return v;
    }
    if (isdigit((unsigned char)*c->p)) {
        const char *start = c->p;
        while (isalnum((unsigned char)*c->p) || *c->p == '_' ||
               *c->p == '\'')
            c->p++;
        int64_t v = 0;
        char *tok = sand_strndup(start, (size_t)(c->p - start));
        int ok = tok && sand_parse_integer(tok, &v);
        free(tok);
        if (!ok) {
            c->ok = 0;
            return 0;
        }
        return v;
    }
    if (isalpha((unsigned char)*c->p) || *c->p == '_' || *c->p == '.' ||
        *c->p == '$') {
        const char *start = c->p;
        while (isalnum((unsigned char)*c->p) || *c->p == '_' ||
               *c->p == '.' || *c->p == '$')
            c->p++;
        char *name = sand_strndup(start, (size_t)(c->p - start));
        sand_symbol_t *s = name ? sand_find_symbol(c->ctx, name) : NULL;
        free(name);
        if (!s || !s->defined || (c->ctx->relocatable && strcmp(s->section, "*abs*"))) {
            c->ok = 0;
            return 0;
        }
        return (int64_t)s->address;
    }
    c->ok = 0;
    return 0;
}

static int64_t expr_mul(expr_cursor_t *c) {
    int64_t v = expr_primary(c);
    for (;;) {
        expr_ws(c);
        if (*c->p == '*') {
            c->p++;
            int64_t d = expr_primary(c);
            if ((v > 0 && ((d > 0 && v > INT64_MAX / d) || (d < 0 && d < INT64_MIN / v))) ||
                (v < 0 && ((d > 0 && v < INT64_MIN / d) || (d < 0 && v < INT64_MAX / d)))) {
                c->ok = 0; return 0;
            }
            v *= d;
        } else if (*c->p == '/') {
            c->p++;
            int64_t d = expr_primary(c);
            if (d == 0 || (v == INT64_MIN && d == -1)) {
                c->ok = 0;
                return 0;
            }
            v /= d;
        } else if (*c->p == '%') {
            c->p++;
            int64_t d = expr_primary(c);
            if (d == 0 || (v == INT64_MIN && d == -1)) {
                c->ok = 0;
                return 0;
            }
            v %= d;
        } else {
            return v;
        }
    }
}

static int64_t expr_add(expr_cursor_t *c) {
    int64_t v = expr_mul(c);
    for (;;) {
        expr_ws(c);
        if (*c->p == '+') {
            c->p++;
            int64_t d = expr_mul(c);
            if ((d > 0 && v > INT64_MAX - d) || (d < 0 && v < INT64_MIN - d)) { c->ok = 0; return 0; }
            v += d;
        } else if (*c->p == '-') {
            c->p++;
            int64_t d = expr_mul(c);
            if ((d < 0 && v > INT64_MAX + d) || (d > 0 && v < INT64_MIN + d)) { c->ok = 0; return 0; }
            v -= d;
        } else {
            return v;
        }
    }
}

static int64_t expr_shift(expr_cursor_t *c) {
    int64_t v=expr_add(c);
    for(;;){expr_ws(c);if(!strncmp(c->p,"<<",2)){c->p+=2;int64_t n=expr_add(c);if(n<0||n>=64)c->ok=0;else v=(int64_t)((uint64_t)v<<n);}
        else if(!strncmp(c->p,">>",2)){c->p+=2;int64_t n=expr_add(c);if(n<0||n>=64)c->ok=0;else v>>=n;}else return v;}
}
static int64_t expr_relational(expr_cursor_t*c){int64_t v=expr_shift(c);for(;;){expr_ws(c);const char*op=c->p;int n=0,kind=0;
    if(!strncmp(op,"<=",2)){n=2;kind=1;}else if(!strncmp(op,">=",2)){n=2;kind=2;}else if(*op=='<'&&op[1]!='<'){n=1;kind=3;}else if(*op=='>'&&op[1]!='>'){n=1;kind=4;}else return v;
    c->p+=n;int64_t r=expr_shift(c);switch(kind){case 1:v=v<=r;break;case 2:v=v>=r;break;case 3:v=v<r;break;default:v=v>r;break;}}}
static int64_t expr_equality(expr_cursor_t*c){int64_t v=expr_relational(c);for(;;){expr_ws(c);if(!strncmp(c->p,"==",2)){c->p+=2;v=v==expr_relational(c);}else if(!strncmp(c->p,"!=",2)){c->p+=2;v=v!=expr_relational(c);}else return v;}}
static int64_t expr_bitand(expr_cursor_t*c){int64_t v=expr_equality(c);for(;;){expr_ws(c);if(*c->p=='&'&&c->p[1]!='&'){c->p++;v&=expr_equality(c);}else return v;}}
static int64_t expr_bitxor(expr_cursor_t*c){int64_t v=expr_bitand(c);for(;;){expr_ws(c);if(*c->p=='^'){c->p++;v^=expr_bitand(c);}else return v;}}
static int64_t expr_bitor(expr_cursor_t*c){int64_t v=expr_bitxor(c);for(;;){expr_ws(c);if(*c->p=='|'&&c->p[1]!='|'){c->p++;v|=expr_bitxor(c);}else return v;}}
static int64_t expr_logic_and(expr_cursor_t*c){int64_t v=expr_bitor(c);for(;;){expr_ws(c);if(!strncmp(c->p,"&&",2)){c->p+=2;int64_t r=expr_bitor(c);v=(v!=0)&&(r!=0);}else return v;}}
static int64_t expr_logic(expr_cursor_t*c){int64_t v=expr_logic_and(c);for(;;){expr_ws(c);if(!strncmp(c->p,"||",2)){c->p+=2;int64_t r=expr_logic_and(c);v=(v!=0)||(r!=0);}else return v;}}

int64_t sand_eval_expr(sandstone_asm_t *ctx, const char *expr, int *ok) {
    expr_cursor_t c;
    c.ctx = ctx;
    c.p = expr ? expr : "";
    c.ok = 1;
    int64_t v = expr_logic(&c);
    expr_ws(&c);
    if (*c.p != '\0')
        c.ok = 0;
    if (ok)
        *ok = c.ok;
    return v;
}

/* ---- directive classification ---------------------------------------- */

static int name_is(const char *mnemonic, const char *name) {
    return strcmp(mnemonic, name) == 0;
}

/* Section-switch directive? On success sets *name_out (static or line
 * operand) and *is_bss_out. */
int sand_directive_section(const sand_line_t *line, const char **name_out,
                           int *is_bss_out) {
    if (line->kind != SAND_LINE_DIRECTIVE)
        return 0;
    const char *m = line->mnemonic;
    if (name_is(m, ".text")) {
        *name_out = ".text";
        *is_bss_out = 0;
        return 1;
    }
    if (name_is(m, ".data")) {
        *name_out = ".data";
        *is_bss_out = 0;
        return 1;
    }
    if (name_is(m, ".rodata")) {
        *name_out = ".rodata";
        *is_bss_out = 0;
        return 1;
    }
    if (name_is(m, ".bss")) {
        *name_out = ".bss";
        *is_bss_out = 1;
        return 1;
    }
    if (name_is(m, ".section") && line->n_operands >= 1 &&
        line->operands[0].sval) {
        *name_out = line->operands[0].sval;
        *is_bss_out = strcmp(*name_out, ".bss") == 0;
        return 1;
    }
    return 0;
}

int sand_directive_is_symbol(const sand_line_t *line) {
    if (line->kind != SAND_LINE_DIRECTIVE)
        return 0;
    const char *m = line->mnemonic;
    return name_is(m, ".globl") || name_is(m, ".global") ||
           name_is(m, ".extern") || name_is(m, ".local") || name_is(m, ".hidden") ||
           name_is(m, ".equ") || name_is(m, ".set") || name_is(m, ".type") ||
           name_is(m, ".size");
}

/* Apply a symbol directive (pass 1). `.equ`/`.set` may defer to a later
 * fixpoint round when the expression names symbols that are not defined
 * yet; redefinition within one round is an error. */
int sand_directive_symbol(sandstone_asm_t *ctx, const sand_line_t *line,
                          int round) {
    const char *m = line->mnemonic;
    if (name_is(m, ".type") || name_is(m, ".size"))
        return 1; /* accepted, no local effect */
    if (name_is(m, ".globl") || name_is(m, ".global") ||
        name_is(m, ".extern") || name_is(m, ".local") || name_is(m, ".hidden")) {
        for (size_t i = 0; i < line->n_operands; i++) {
            const char *name =
                line->operands[i].sval ? line->operands[i].sval : "";
            if (!*name) {
                sand_set_error(ctx, "%s:%u: %s needs a symbol name",
                               line->src_name, line->lineno, m);
                return 0;
            }
            sand_symbol_t *s = sand_find_symbol(ctx, name);
            if (!s) {
                /* create undefined entry */
                sand_symbol_t ns;
                memset(&ns, 0, sizeof ns);
                ns.name = sand_strdup(name);
                ns.section = sand_strdup("");
                if (!ns.name || !ns.section) {
                    free(ns.name);
                    free(ns.section);
                    return 0;
                }
                if (ctx->n_symbols >= SIZE_MAX / sizeof ns)
                    return 0;
                sand_symbol_t *arr = sand_realloc_array(
                    ctx->symbols, ctx->n_symbols + 1, sizeof ns);
                if (!arr)
                    return 0;
                ctx->symbols = arr;
                ctx->symbols[ctx->n_symbols++] = ns;
                s = &ctx->symbols[ctx->n_symbols - 1];
            }
            if (name_is(m, ".hidden")) s->hidden = 1;
            if (name_is(m, ".globl") || name_is(m, ".global")) {
                s->global = 1;
                s->explicit_local = 0;
            }
            if (name_is(m, ".local")) {
                s->explicit_local = 1;
                s->global = 0;
            }
        }
        return 1;
    }
    if (name_is(m, ".equ") || name_is(m, ".set")) {
        if (line->n_operands < 2 || !line->operands[0].sval ||
            !line->operands[1].sval) {
            sand_set_error(ctx, "%s:%u: %s needs NAME, EXPR", line->src_name,
                           line->lineno, m);
            return 0;
        }
        int ok = 0;
        int64_t v = sand_eval_expr(ctx, line->operands[1].sval, &ok);
        if (!ok)
            return 1; /* fixpoint loop retries later */
        sand_symbol_t *s = sand_find_symbol(ctx, line->operands[0].sval);
        if (!s) {
            sand_symbol_t ns;
            memset(&ns, 0, sizeof ns);
            ns.name = sand_strdup(line->operands[0].sval);
            ns.section = sand_strdup("*abs*");
            if (!ns.name || !ns.section) {
                free(ns.name);
                free(ns.section);
                return 0;
            }
            sand_symbol_t *arr = sand_realloc_array(
                ctx->symbols, ctx->n_symbols + 1, sizeof ns);
            if (!arr)
                return 0;
            ctx->symbols = arr;
            ctx->symbols[ctx->n_symbols++] = ns;
            s = &ctx->symbols[ctx->n_symbols - 1];
        }
        if (s->defined && s->round == round &&
            (int64_t)s->address != v && name_is(m, ".equ")) {
            sand_set_error(ctx, "%s:%u: .equ redefines '%s'", line->src_name,
                           line->lineno, s->name);
            return 0;
        }
        s->address = (uint64_t)v;
        s->defined = 1;
        s->round = round;
        free(s->section);
        s->section = sand_strdup("*abs*");
        return s->section != NULL;
    }
    sand_set_error(ctx, "%s:%u: unknown directive '%s'", line->src_name,
                   line->lineno, m);
    return 0;
}

/* ---- data directive sizes (pass 1) ------------------------------------ */

static int operand_count_ok(sandstone_asm_t *ctx, const sand_line_t *line,
                            size_t min) {
    if (line->n_operands < min) {
        sand_set_error(ctx, "%s:%u: %s needs an argument", line->src_name,
                       line->lineno, line->mnemonic);
        return 0;
    }
    return 1;
}

/* One decoder drives both sizing and emission, including octal/hex escapes. */
static int decode_string(const char *s, uint8_t *out, size_t *len_out) {
    if (!s || s[0] != '"')
        return 0;
    size_t n = 0;
    for (s++; *s && *s != '"'; s++) {
        unsigned value = (unsigned char)*s;
        if (*s == '\\') {
            ++s;
            if (!*s) return 0;
            if (*s == 'x') {
                value = 0; unsigned digits = 0;
                while (isxdigit((unsigned char)s[1])) {
                    ++s; unsigned d = isdigit((unsigned char)*s) ? (unsigned)(*s - '0') : (unsigned)(tolower((unsigned char)*s) - 'a' + 10);
                    if (value > (255 - d) / 16) return 0;
                    value = value * 16 + d; ++digits;
                }
                if (!digits) return 0;
            } else if (*s >= '0' && *s <= '7') {
                value = (unsigned)(*s - '0');
                for (unsigned i = 1; i < 3 && s[1] >= '0' && s[1] <= '7'; ++i) value = value * 8 + (unsigned)(*++s - '0');
                if (value > 255) return 0;
            } else switch (*s) {
                case 'n': value = '\n'; break; case 'r': value = '\r'; break;
                case 't': value = '\t'; break; case 'b': value = '\b'; break;
                case 'f': value = '\f'; break; case 'v': value = '\v'; break;
                case 'a': value = '\a'; break; case '\\': value = '\\'; break;
                case '"': value = '"'; break; default: return 0;
            }
        }
        if (out) out[n] = (uint8_t)value;
        ++n;
    }
    if (*s != '"' || s[1])
        return 0;
    *len_out = n;
    return 1;
}

/* Alignment directive value: .align N is 2^N on GAS/x86, bytes elsewhere.
 * We follow GAS: a small N (<= 16) on a byte-aligned target means 2^N;
 * use .balign for explicit byte counts. */
static uint64_t align_value(const char *mnemonic, int64_t n) {
    if (n < 0)
        return 0;
    if (strcmp(mnemonic, ".p2align") == 0)
        return (uint64_t)n <= 30 ? (1u << (uint64_t)n) : 0;
    if (strcmp(mnemonic, ".balign") == 0)
        return (uint64_t)n;
    /* .align: power-of-two exponent for N <= 16, bytes above. */
    if (n <= 16)
        return 1u << (uint64_t)n;
    return (uint64_t)n;
}

int sand_directive_size(sandstone_asm_t *ctx, const sand_line_t *line,
                        uint32_t *size_out) {
    const char *m = line->mnemonic;
    *size_out = 0;
    if (name_is(m, ".byte"))
        *size_out = (uint32_t)line->n_operands * 1;
    else if (name_is(m, ".short") || name_is(m, ".hword") || name_is(m, ".2byte"))
        *size_out = (uint32_t)line->n_operands * 2;
    else if (name_is(m, ".word") || name_is(m, ".long") || name_is(m, ".4byte"))
        *size_out = (uint32_t)line->n_operands * 4;
    else if (name_is(m, ".quad") || name_is(m, ".8byte"))
        *size_out = (uint32_t)line->n_operands * 8;
    else if (name_is(m, ".ascii") || name_is(m, ".asciz") ||
             name_is(m, ".string")) {
        size_t total = 0;
        for (size_t i = 0; i < line->n_operands; i++) {
            size_t l = 0;
            if (!line->operands[i].sval ||
                 !decode_string(line->operands[i].sval, NULL, &l)) {
                sand_set_error(ctx, "%s:%u: %s needs quoted strings",
                               line->src_name, line->lineno, m);
                return 0;
            }
            total += l;
            if (!name_is(m, ".ascii")) ++total;
        }
        if (total > UINT32_MAX) return 0;
        *size_out = (uint32_t)total;
        return 1;
    } else if (name_is(m, ".zero") || name_is(m, ".space") ||
               name_is(m, ".skip")) {
        if (!operand_count_ok(ctx, line, 1))
            return 0;
        int ok = 0;
        int64_t n = sand_eval_expr(
            ctx, line->operands[0].sval ? line->operands[0].sval : "", &ok);
        if (!ok || n < 0 || n > 16 * 1024 * 1024) {
            sand_set_error(ctx, "%s:%u: %s needs a constant count",
                           line->src_name, line->lineno, m);
            return 0;
        }
        *size_out = (uint32_t)n;
        return 1;
    } else if (name_is(m, ".fill")) {
        if (line->n_operands < 3) {
            sand_set_error(ctx, "%s:%u: .fill needs COUNT,SIZE,VALUE",
                           line->src_name, line->lineno);
            return 0;
        }
        int ok = 0;
        int64_t count = sand_eval_expr(ctx, line->operands[0].sval, &ok);
        int ok2 = 0;
        int64_t size =
            sand_eval_expr(ctx, line->operands[1].sval, &ok2);
        if (!ok || !ok2 || count < 0 || size < 0 || size > 8 || (size && count > 16 * 1024 * 1024 / size)) {
            sand_set_error(ctx, "%s:%u: .fill needs constant COUNT,SIZE",
                           line->src_name, line->lineno);
            return 0;
        }
        *size_out = (uint32_t)(count * size);
        return 1;
    } else if (name_is(m, ".uleb128") || name_is(m, ".sleb128")) {
        if (!operand_count_ok(ctx, line, 1)) return 0;
        for (size_t i = 0; i < line->n_operands; ++i) {
            int ok = 0; int64_t value = sand_eval_expr(ctx, line->operands[i].sval, &ok);
            int sign = name_is(m, ".sleb128");
            if (!ok || (!sign && value < 0)) { sand_set_error(ctx, "invalid LEB128 constant"); return 0; }
            int more;
            do {
                unsigned byte = (unsigned)((uint64_t)value & 127); value >>= 7;
                more = sign ? !((value == 0 && !(byte & 64)) || (value == -1 && (byte & 64))) : value != 0;
                ++*size_out;
            } while (more);
        }
        return 1;
    } else if (name_is(m, ".align") || name_is(m, ".balign") ||
               name_is(m, ".p2align") || name_is(m, ".org")) {
        /* Size depends on the address; the binarizer pads directly and
         * assigns size 0 here (padding accounted at layout time). */
        *size_out = 0;
        return 1;
    } else {
        sand_set_error(ctx, "%s:%u: unknown directive '%s'", line->src_name,
                       line->lineno, m);
        return 0;
    }
    if (line->kind == SAND_LINE_DIRECTIVE && line->n_operands == 0 &&
        !name_is(m, ".ascii") && !name_is(m, ".asciz") &&
        !name_is(m, ".string")) {
        sand_set_error(ctx, "%s:%u: %s needs an argument", line->src_name,
                       line->lineno, m);
        return 0;
    }
    return 1;
}

/* ---- data directive emission (pass 2) ---------------------------------- */

static int put_u(sand_section_t *sec, uint64_t v, unsigned n, int be) {
    if (sec->len + n > sec->cap) {
        size_t ncap = sec->cap ? sec->cap * 2 : 64;
        while (ncap < sec->len + n)
            ncap *= 2;
        uint8_t *next = realloc(sec->bytes, ncap);
        if (!next) return 0;
        sec->bytes = next;
        sec->cap = ncap;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned sh = be ? (n - 1 - i) * 8 : i * 8;
        sec->bytes[sec->len++] = (uint8_t)((v >> sh) & 0xFF);
    }
    return 1;
}

static int put_checked(sandstone_asm_t *ctx, sand_section_t *sec, int64_t v, unsigned width) {
    if (width < 8 && (v < -(INT64_C(1) << (width * 8 - 1)) ||
        (v >= 0 && (uint64_t)v >= (UINT64_C(1) << (width * 8))))) {
        sand_set_error(ctx, "data value out of range for %u-bit field", width * 8);
        return 0;
    }
    return put_u(sec, (uint64_t)v, width, ctx->byte_order);
}

/* Append a relocation for a directive-emitted absolute field. */
static int add_abs_reloc(sandstone_asm_t *ctx, uint32_t sec_idx,
                         uint32_t offset, const char *symbol, int64_t addend,
                         uint32_t width_bits) {
    sand_reloc_intern_t *arr = sand_realloc_array(ctx->relocs,
                                                 ctx->n_relocs + 1,
                                                 sizeof *arr);
    if (!arr)
        return 0;
    ctx->relocs = arr;
    sand_reloc_intern_t *r = &ctx->relocs[ctx->n_relocs++];
    memset(r, 0, sizeof *r);
    r->sec_idx = sec_idx;
    r->offset = offset;
    r->symbol = sand_strdup(symbol);
    r->addend = addend;
    r->type = sand_strdup("ABS");
    r->width_bits = width_bits;
    r->pcrel = 0;
    return r->symbol && r->type;
}

/* Emit one EXPR of `width` bytes; symbols become relocations/addends. */
static int emit_expr(sandstone_asm_t *ctx, const sand_line_t *line,
                     const char *expr, unsigned width, uint32_t sec_idx) {
    /* Fast path: plain symbol name -> absolute relocation. */
    sand_section_t *sec = &ctx->sections[sec_idx];
    if (sand_is_name(expr)) {
        sand_symbol_t *s = sand_find_symbol(ctx, expr);
        if (s && s->defined && (!ctx->relocatable || !strcmp(s->section, "*abs*"))) {
            return put_checked(ctx, sec, (int64_t)s->address, width);
        }
        uint32_t at = (uint32_t)sec->len;
        if (!put_u(sec, 0, width, ctx->byte_order)) return 0;
        if (!add_abs_reloc(ctx, sec_idx, at, expr, 0, width * 8))
            return 0;
        return 1;
    }
    /* General expression: evaluate; on undefined symbols keep a reloc
     * when the expression is exactly `symbol + constant`. */
    int ok = 0;
    int64_t v = sand_eval_expr(ctx, expr, &ok);
    if (ok) {
        return put_checked(ctx, sec, v, width);
    }
    /* symbol +/- constant? */
    char *plus = strrchr((char *)expr, '+');
    char *minus = strrchr((char *)expr, '-');
    char *sep = NULL;
    if (plus && (!minus || plus > minus))
        sep = plus;
    else if (minus)
        sep = minus;
    if (sep && sep != expr) {
        char *name = sand_strndup(expr, (size_t)(sep - expr));
        char *trimmed = name ? sand_trim(name) : NULL;
        int64_t add = 0;
        int ok2 = 0;
        if (trimmed && sand_is_name(trimmed))
            add = sand_eval_expr(ctx, sep + 1, &ok2);
        if (sep == minus && add == INT64_MIN) ok2 = 0;
        if (sep == minus && ok2)
            add = -add;
        if (ok2) {
            sand_symbol_t *s = sand_find_symbol(ctx, trimmed);
            if (s && s->defined && (!ctx->relocatable || !strcmp(s->section, "*abs*"))) {
                int result = put_checked(ctx, sec, (int64_t)((uint64_t)s->address + (uint64_t)add), width);
                free(name);
                return result;
            }
            uint32_t at = (uint32_t)sec->len;
            if (!put_u(sec, 0, width, ctx->byte_order)) { free(name); return 0; }
            int rc = add_abs_reloc(ctx, sec_idx, at, trimmed, add,
                                   width * 8);
            free(name);
            return rc;
        }
        free(name);
    }
    sand_set_error(ctx, "%s:%u: cannot evaluate '%s'", line->src_name,
                   line->lineno, expr);
    return 0;
}

static int emit_string(sand_section_t *sec, const char *s) {
    size_t n = 0;
    if (!decode_string(s, NULL, &n) || n > SIZE_MAX - sec->len) return 0;
    if (sec->len + n > sec->cap) {
        uint8_t *next = realloc(sec->bytes, sec->len + n);
        if (!next) return 0;
        sec->bytes = next; sec->cap = sec->len + n;
    }
    if (n && !decode_string(s, sec->bytes + sec->len, &n)) return 0;
    sec->len += n;
    return 1;
}

int sand_emit_directive(sandstone_asm_t *ctx, const sand_line_t *line,
                        uint32_t sec_idx, uint64_t address) {
    (void)address;
    const char *m = line->mnemonic;
    sand_section_t *sec = &ctx->sections[sec_idx];
    unsigned width = 0;
    if (name_is(m, ".byte"))
        width = 1;
    else if (name_is(m, ".short") || name_is(m, ".hword") || name_is(m, ".2byte"))
        width = 2;
    else if (name_is(m, ".word") || name_is(m, ".long") || name_is(m, ".4byte"))
        width = 4;
    else if (name_is(m, ".quad") || name_is(m, ".8byte"))
        width = 8;
    if (width) {
        for (size_t i = 0; i < line->n_operands; i++) {
            if (!emit_expr(ctx, line,
                           line->operands[i].sval ? line->operands[i].sval
                                                  : "",
                           width, sec_idx))
                return 0;
        }
        return 1;
    }
    if (name_is(m, ".ascii") || name_is(m, ".asciz") ||
        name_is(m, ".string")) {
        for (size_t i = 0; i < line->n_operands; i++) {
            if (!emit_string(sec, line->operands[i].sval)) {
                sand_set_error(ctx, "%s:%u: %s needs quoted strings",
                               line->src_name, line->lineno, m);
                return 0;
            }
            if (!name_is(m, ".ascii") && !put_u(sec, 0, 1, 0)) return 0;
        }
        return 1;
    }
    if (name_is(m, ".zero") || name_is(m, ".space") ||
        name_is(m, ".skip")) {
        int ok = 0;
        int64_t n = sand_eval_expr(ctx, line->operands[0].sval, &ok);
        int64_t fill = 0;
        if (line->n_operands > 1) {
            int ok2 = 0;
            fill = sand_eval_expr(ctx, line->operands[1].sval, &ok2);
            if (!ok2)
                ok = 0;
        }
        if (!ok || n < 0) {
            sand_set_error(ctx, "%s:%u: %s needs a constant count",
                           line->src_name, line->lineno, m);
            return 0;
        }
        if (ctx->relocatable && sec->is_bss) {
            if (fill) {
                sand_set_error(ctx, "%s:%u: nonzero initializer in NOBITS section", line->src_name, line->lineno);
                return 0;
            }
            return 1;
        }
        for (int64_t i = 0; i < n; i++)
            if (!put_u(sec, (uint64_t)fill, 1, 0)) return 0;
        return 1;
    }
    if (name_is(m, ".fill")) {
        int ok = 0, ok2 = 0, ok3 = 0;
        int64_t count = sand_eval_expr(ctx, line->operands[0].sval, &ok);
        int64_t size = sand_eval_expr(ctx, line->operands[1].sval, &ok2);
        int64_t value = sand_eval_expr(ctx, line->operands[2].sval, &ok3);
        if (!ok || !ok2 || !ok3 || count < 0 || size < 0 || size > 8) {
            sand_set_error(ctx, "%s:%u: invalid .fill arguments",
                           line->src_name, line->lineno);
            return 0;
        }
        for (int64_t i = 0; i < count; i++)
            if (!put_u(sec, (uint64_t)value, (unsigned)size, ctx->byte_order)) return 0;
        return 1;
    }
    if (name_is(m, ".uleb128") || name_is(m, ".sleb128")) {
        for (size_t i = 0; i < line->n_operands; ++i) {
            int ok = 0; int64_t value = sand_eval_expr(ctx, line->operands[i].sval, &ok);
            int sign = name_is(m, ".sleb128"), more;
            if (!ok || (!sign && value < 0)) return 0;
            do {
                unsigned byte = (unsigned)((uint64_t)value & 127); value >>= 7;
                more = sign ? !((value == 0 && !(byte & 64)) || (value == -1 && (byte & 64))) : value != 0;
                if (!put_u(sec, byte | (more ? 128 : 0), 1, 0)) return 0;
            } while (more);
        }
        return 1;
    }
    sand_set_error(ctx, "%s:%u: cannot emit '%s' here", line->src_name,
                   line->lineno, m);
    return 0;
}

/* Alignment/origin padding. With emit=0 only the pad length is computed
 * (added to the line size by the caller via *pad_out semantics here:
 * returns the pad length, or -1 on error). */
int sand_directive_pad(sandstone_asm_t *ctx, const sand_line_t *line,
                       uint32_t sec_idx, uint64_t address, int emit) {
    const char *m = line->mnemonic;
    int ok = 0;
    int64_t arg = line->n_operands >= 1
                      ? sand_eval_expr(ctx, line->operands[0].sval, &ok)
                      : 0;
    if (line->n_operands < 1 || !ok) {
        sand_set_error(ctx, "%s:%u: %s needs a constant argument",
                       line->src_name, line->lineno, m);
        return -1;
    }
    int64_t fill = 0;
    if (line->n_operands > 1) {
        int ok2 = 0;
        fill = sand_eval_expr(ctx, line->operands[1].sval, &ok2);
        if (!ok2) {
            sand_set_error(ctx, "%s:%u: %s needs a constant fill",
                           line->src_name, line->lineno, m);
            return -1;
        }
    }
    uint64_t want = 0;
    if (name_is(m, ".org")) {
        if (arg < 0 || (uint64_t)arg < address) {
            sand_set_error(ctx, "%s:%u: .org moves backwards",
                           line->src_name, line->lineno);
            return -1;
        }
        want = (uint64_t)arg - address;
    } else {
        uint64_t align = align_value(m, arg);
        if (align == 0 || (ctx->relocatable && ((align & (align-1)) || align > UINT32_MAX))) {
            sand_set_error(ctx, "%s:%u: invalid alignment", line->src_name,
                           line->lineno);
            return -1;
        }
        if (emit && ctx->relocatable && align > ctx->sections[sec_idx].align)
            ctx->sections[sec_idx].align = (uint32_t)align;
        uint64_t mis = address % align;
        want = mis ? align - mis : 0;
    }
    if (want > 0x1000000) {
        sand_set_error(ctx, "%s:%u: padding exceeds 16 MiB", line->src_name,
                       line->lineno);
        return -1;
    }
    if (emit && ctx->relocatable && ctx->sections[sec_idx].is_bss) {
        if (fill && want) {
            sand_set_error(ctx, "%s:%u: nonzero padding in NOBITS section", line->src_name, line->lineno);
            return -1;
        }
    } else if (emit && want) {
        sand_section_t *sec = &ctx->sections[sec_idx];
        const DomSymAs_Target *target = domsymas_target_at(ctx->bank, 0);
        if (target && !strcmp(target->arch, "aarch64") && !strcmp(sec->name, ".text") &&
            !name_is(m, ".org") && line->n_operands == 1 && !(address % 4) && !(want % 4)) {
            for (uint64_t i = 0; i < want; i += 4)
                if (!put_u(sec, UINT32_C(0xd503201f), 4, 0)) return -1;
            return (int)want;
        }
        uint8_t fb = (uint8_t)(fill & 0xFF);
        for (uint64_t i = 0; i < want; i++)
            if (!put_u(sec, fb, 1, 0)) return -1;
    }
    return (int)want;
}
