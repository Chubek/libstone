/* Standalone acsem @VERSION@ interpreter (C99 or C++11). MIT-0.
 * Both targets share this portable kernel and need only the standard library.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

static const char *const model_data[] = {
@MODEL@
};

typedef struct { int type; int64_t n; } Value;
typedef struct { int key; Value value; } Binding;
typedef struct { int count; Value items[64]; } Values;
typedef struct { int count; Binding items[64]; } Env;
typedef struct { int status; Values out; Env bindings; } Result;
typedef struct { char name[65]; int sort, count, rule, types[16]; } Constructor;
typedef struct { int op, x, y, n; char text[65]; } Instruction;
typedef struct { int ctor; Value fields[16]; } Term;
typedef struct { char text[65]; int kind; size_t at; } Token;

static Constructor ctors[256];
static Instruction code[4096];
static Term terms[2048];
static Token tokens[20001];
static Value cells[1024];
static char symbols[256][65], source[65538];
static int start_sort, ctor_count, term_count, token_count, pos;
static int cell_count, symbol_count, steps;

static void die(const char *prefix, const char *message) {
    fprintf(stderr, "%s: %s\n", prefix, message);
    exit(2);
}
static void error(const char *message) { die("ACSEM_ERROR", message); }
static void limit(const char *message) { die("ACSEM_LIMIT", message); }
static void syntax_at(const char *message, size_t at) {
    size_t i, line = 1, col = 1;
    for (i = 0; i < at; ++i) {
        if (source[i] == '\n') { ++line; col = 1; } else { ++col; }
    }
    fprintf(stderr, "ACSEM_ERROR: %s at %lu:%lu\n", message,
            (unsigned long)line, (unsigned long)col);
    exit(2);
}
static void syntax(const char *message) { syntax_at(message, tokens[pos].at); }
static int letter(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int digit(int c) { return c >= '0' && c <= '9'; }
static Value value(int type, int64_t n) { Value v; v.type = type; v.n = n; return v; }
static int symbol(const char *text) {
    int i;
    for (i = 0; i < symbol_count; ++i) if (!strcmp(symbols[i], text)) return i;
    if (symbol_count == 256) limit("runtime symbol limit");
    strcpy(symbols[symbol_count], text);
    return symbol_count++;
}
static int number(const char **p) {
    char *end;
    long n = strtol(*p, &end, 10);
    *p = end;
    return (int)n;
}
static void word(const char **p, char *out) {
    int i = 0;
    while (**p == ' ') ++*p;
    while (**p && **p != ' ') out[i++] = *(*p)++;
    out[i] = '\0';
}
static void load_model(void) {
    int i, j, nn;
    const char *p = model_data[0];
    start_sort = number(&p); ctor_count = number(&p); nn = number(&p);
    for (i = 0; i < ctor_count; ++i) {
        Constructor *c = &ctors[i]; p = model_data[i + 1];
        word(&p, c->name); c->sort = number(&p); c->count = number(&p); c->rule = number(&p);
        for (j = 0; j < c->count; ++j) c->types[j] = number(&p);
    }
    for (i = 0; i < nn; ++i) {
        Instruction *a = &code[i]; p = model_data[ctor_count + i + 1];
        a->op = number(&p); a->x = number(&p); a->y = number(&p); a->n = number(&p);
        word(&p, a->text);
    }
}
static void lex(size_t length) {
    size_t at = 0;
    while (at < length) {
        size_t begin = at, size;
        int kind = 3;
        unsigned char c = (unsigned char)source[at];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++at; continue; }
        if (letter(c)) {
            kind = 1;
            do { ++at; } while (at < length && (letter(source[at]) || digit(source[at])));
        } else if (digit(c)) {
            kind = 2;
            do { ++at; } while (at < length && digit(source[at]));
        } else if (c && strchr("{}(),;:=|+*/-<", c)) {
            ++at;
            if (c == '=' && at < length && source[at] == '=') ++at;
        } else { syntax_at(c ? "invalid term syntax" : "NUL in source", at); }
        size = at - begin;
        if (size > 64) limit("source token exceeds 64 bytes");
        if (token_count == 20000) limit("too many tokens");
        memcpy(tokens[token_count].text, source + begin, size);
        tokens[token_count].text[size] = '\0';
        tokens[token_count].kind = kind; tokens[token_count++].at = begin;
    }
    strcpy(tokens[token_count].text, "<eof>"); tokens[token_count].at = length;
}
static int eat(const char *text) {
    if (strcmp(tokens[pos].text, text)) return 0;
    ++pos; return 1;
}
static void expect(const char *text) {
    if (!eat(text)) {
        char message[96]; snprintf(message, sizeof(message), "expected '%s'", text); syntax(message);
    }
}
static int parse_term(int sort, int depth) {
    int index, ci, i;
    Constructor *c;
    if (depth > 128) limit("parse depth exceeded");
    if (term_count == 2048) limit("program syntax node limit");
    index = term_count++;
    if (tokens[pos].kind != 1) syntax("expected identifier");
    for (ci = 0; ci < ctor_count; ++ci) if (!strcmp(ctors[ci].name, tokens[pos].text)) break;
    ++pos;
    if (ci == ctor_count) syntax("unknown constructor");
    c = &ctors[ci];
    if (c->sort != sort) syntax("constructor has wrong sort");
    terms[index].ctor = ci;
    expect("(");
    for (i = 0; i < c->count; ++i) {
        if (i) expect(",");
        if (c->types[i] == -1) {
            int negative = eat("-");
            int64_t n;
            const char *digits;
            if (tokens[pos].kind != 2) syntax("integer field expected");
            digits = tokens[pos++].text;
            while (*digits == '0' && digits[1]) ++digits;
            if (strlen(digits) > 10) syntax("integer field outside 32-bit range");
            n = (int64_t)strtoll(digits, NULL, 10);
            if (negative) n = -n;
            if (n < INT32_MIN || n > INT32_MAX) syntax("integer field outside 32-bit range");
            terms[index].fields[i] = value(1, n);
        } else if (c->types[i] == -2) {
            if (tokens[pos].kind != 1) syntax("expected identifier");
            terms[index].fields[i] = value(2, symbol(tokens[pos++].text));
        } else { terms[index].fields[i] = value(4, parse_term(c->types[i], depth + 1)); }
    }
    expect(")"); return index;
}
static void tick(int depth) {
    if (++steps > 100000) limit("evaluation step budget exceeded");
    if (depth > 128) limit("evaluation nesting exceeded");
}
static Value lookup(const Env *env, int key) {
    int i;
    for (i = 0; i < env->count; ++i) if (env->items[i].key == key) return env->items[i].value;
    return value(0, 0);
}
static Value expr(int pc, int term, const Values *inputs, const Env *env, int depth) {
    Instruction *e = &code[pc];
    Value a, b;
    int64_t n = 0;
    tick(depth);
    if (e->op == 1) return value(1, e->n);
    if (e->op == 2) return value(2, symbol(e->text));
    if (e->op == 3) return terms[term].fields[e->n];
    if (e->op == 4) return e->n < inputs->count ? inputs->items[e->n] : value(0, 0);
    a = expr(e->x, term, inputs, env, depth + 1);
    if (e->op == 5) return a.type == 2 ? lookup(env, (int)a.n) : value(0, 0);
    if (e->op == 6) return a.type == 3 && a.n >= 0 && a.n < cell_count ? cells[a.n] : value(0, 0);
    b = expr(e->y, term, inputs, env, depth + 1);
    if (a.type != 1 || b.type != 1) return value(0, 0);
    switch (e->op) {
        case 7: n = a.n + b.n; break;
        case 8: n = a.n - b.n; break;
        case 9: n = a.n * b.n; break;
        case 10:
            if (!b.n || (a.n == INT32_MIN && b.n == -1)) return value(0, 0);
            n = a.n / b.n; break;
        case 11: n = a.n == b.n; break;
        case 12: n = a.n < b.n; break;
        default: error("invalid expression instruction");
    }
    return n < INT32_MIN || n > INT32_MAX ? value(0, 0) : value(1, n);
}
static Result result(int status) {
    Result r; memset(&r, 0, sizeof(r)); r.status = status; return r;
}
static int merge(Env *out, const Env *a, const Env *b, int overlay) {
    int i, j, count = a->count;
    for (i = 0; i < b->count; ++i) {
        int found = lookup(a, b->items[i].key).type != 0;
        if (found && !overlay) return 0;
        if (!found) ++count;
    }
    if (count > 64) limit("binding result capacity");
    *out = *a;
    for (i = 0; i < b->count; ++i) {
        for (j = 0; j < out->count; ++j) if (out->items[j].key == b->items[i].key) break;
        if (j == out->count) ++out->count;
        out->items[j] = b->items[i];
    }
    return 1;
}
static Result run_node(int term, const Values *inputs, const Env *env, int depth);
static Result action(int pc, int term, const Values *inputs, const Env *env, int depth) {
    Instruction *a = &code[pc];
    Result left, right, r;
    Value v, w;
    int i;
    tick(depth);
    if (a->op == 13) return result(0);
    if (a->op == 14) return result(1);
    if (a->op >= 15 && a->op <= 17) {
        v = expr(a->x, term, inputs, env, depth + 1);
        if (!v.type) return result(1);
        if (a->op == 16) return result(v.type == 1 && v.n ? 0 : 1);
        r = result(a->op == 17 ? 2 : 0); r.out.items[r.out.count++] = v; return r;
    }
    if (a->op == 19) {
        if (cell_count == 1024) limit("allocated cell limit");
        r = result(0); r.out.items[r.out.count++] = value(3, cell_count++); return r;
    }
    if (a->op == 18 || a->op == 20) {
        v = expr(a->x, term, inputs, env, depth + 1);
        w = expr(a->y, term, inputs, env, depth + 1);
        if (a->op == 18) {
            if (v.type != 2 || !w.type) return result(1);
            r = result(0); r.bindings.count = 1;
            r.bindings.items[0].key = (int)v.n; r.bindings.items[0].value = w; return r;
        }
        if (v.type != 1 || w.type != 3 || w.n < 0 || w.n >= cell_count) return result(1);
        cells[w.n] = v; return result(0);
    }
    if (a->op == 21) return run_node((int)terms[term].fields[a->n].n, inputs, env, depth + 1);
    left = action(a->x, term, inputs, env, depth + 1);
    if (a->op == 22) {
        if (left.status) return left;
        r = result(0); r.out = left.out; (void)merge(&r.bindings, env, &left.bindings, 1); return r;
    }
    if (a->op == 26) return left.status == 1 ? action(a->y, term, inputs, env, depth + 1) : left;
    if (a->op == 27) return left.status == 2 ? action(a->y, term, &left.out, env, depth + 1) : left;
    if (left.status) return left;
    right = action(a->y, term, a->op == 23 ? &left.out : inputs,
                   a->op == 25 ? &left.bindings : env, depth + 1);
    if (right.status) return right;
    r = result(0);
    if (a->op == 25) r.bindings = right.bindings;
    else if (!merge(&r.bindings, &left.bindings, &right.bindings, 0)) return result(1);
    if (a->op == 23) r.out = right.out;
    else {
        if (left.out.count + right.out.count > 64) limit("transient result capacity");
        r.out = left.out;
        for (i = 0; i < right.out.count; ++i) r.out.items[r.out.count++] = right.out.items[i];
    }
    return r;
}
static Result run_node(int term, const Values *inputs, const Env *env, int depth) {
    tick(depth);
    return action(ctors[terms[term].ctor].rule, term, inputs, env, depth + 1);
}
static size_t read_source(const char *path) {
    FILE *f = path ? fopen(path, "rb") : stdin;
    size_t size;
    if (!f) error("cannot open term file");
    size = fread(source, 1, 65537, f);
    if (ferror(f)) error("input read failed");
    if (path && fclose(f)) error("input close failed");
    if (size > 65536) limit("input exceeds 64 KiB");
    source[size] = '\0'; return size;
}
int main(int argc, char **argv) {
    const char *term = NULL, *path = NULL, *output = NULL;
    size_t size;
    int i, root;
    Result r;
    Values inputs = {0, {{0, 0}}};
    Env env = {0, {{0, {0, 0}}}};
    FILE *f;
    for (i = 1; i < argc; ++i) {
        char flag[32];
        const char *eq = strchr(argv[i], '='), *arg;
        size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (len >= sizeof(flag)) error("invalid option");
        memcpy(flag, argv[i], len); flag[len] = '\0';
        if (!eq && (!strcmp(flag, "--help") || !strcmp(flag, "-h"))) {
            puts("generated interpreter [--term PROGRAM | --term-file PATH] [-o FILE]\nExit: completed=0, failed/escaped=1, errors=2."); return 0;
        }
        if (!eq && !strcmp(flag, "--version")) { puts("acsem @VERSION@"); return 0; }
        if (!eq && !strcmp(flag, "--run")) continue;
        if (strcmp(flag, "--term") && strcmp(flag, "--term-file") && strcmp(flag, "-o") && strcmp(flag, "--output")) error("invalid option");
        if (eq) arg = eq + 1;
        else { if (++i == argc) error("missing option value"); arg = argv[i]; }
        if (!strcmp(flag, "--term")) term = arg;
        else if (!strcmp(flag, "--term-file")) path = arg;
        else output = arg;
    }
    if (term && path) error("use only one of --term and --term-file");
    if (term) {
        size = strlen(term); if (size > 65536) limit("source exceeds 64 KiB");
        memcpy(source, term, size + 1);
    } else size = read_source(path);
    load_model(); lex(size); root = parse_term(start_sort, 1);
    if (tokens[pos].kind) syntax("trailing term input");
    r = run_node(root, &inputs, &env, 1);
    f = output && strcmp(output, "-") ? fopen(output, "wb") : stdout;
    if (!f) error("cannot write output file");
    fputs(r.status == 0 ? "completed" : r.status == 1 ? "failed" : "escaped", f);
    for (i = 0; i < r.out.count; ++i) {
        Value v = r.out.items[i];
        if (v.type == 2) fprintf(f, " token:%s", symbols[v.n]);
        else fprintf(f, " %s%" PRId64, v.type == 3 ? "cell:" : "", v.n);
    }
    fputc('\n', f);
    if (fflush(f) || ferror(f)) error("output write failed");
    if (f != stdout && fclose(f)) error("output close failed");
    return r.status ? 1 : 0;
}
