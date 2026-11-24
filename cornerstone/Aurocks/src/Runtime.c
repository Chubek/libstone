#include "aurocks/Runtime.h"

#include <glr/parser.h>
#include <glr/scannerless.h>

#include <limits.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct parse_context {
    const aurocks_parser_spec *spec;
    const size_t *rule_indices;
    void **values;
    char **captures;
    regex_t skip;
} parse_context;

static size_t skip_trivia(const char *input, size_t length, size_t position, void *opaque) {
    parse_context *context = opaque;
    while (position < length) {
        regmatch_t match;
        if (regexec(&context->skip, input + position, 1, &match, 0) != 0 ||
            match.rm_so != 0 || match.rm_eo <= 0 || (size_t)match.rm_eo > length - position)
            break;
        position += (size_t)match.rm_eo;
    }
    return position;
}

static int reduce(const glr_semantic_context_t *semantic, void **value, void *opaque) {
    parse_context *context = semantic->user_data;
    const aurocks_parser_spec *spec = context->spec;
    const size_t index = *(const size_t *)opaque;
    const aurocks_rule_spec *rule = &spec->rules[index];
    size_t child = 0;
    char *token = NULL;
    memset(context->captures, 0, rule->length * sizeof(*context->captures));
    for (size_t i = 0; i < rule->length; ++i) {
        const int symbol = rule->rhs[i];
        context->values[i] = NULL;
        if (symbol < 0)
            continue;
        if (child >= semantic->selection.child_count)
            goto failure;
        if (spec->symbols[symbol].kind == AUROCKS_NONTERMINAL) {
            context->values[i] = semantic->selection.values[child];
        } else if (spec->terminal_use) {
            const unsigned use = spec->terminal_use(index, i) & 3u;
            if (use) {
                glr_selection_t terminal;
                if (glr_select_position(&semantic->selection, child + 1, &terminal) != 0)
                    goto failure;
                char *text = malloc(terminal.length + 1);
                if (!text) goto failure;
                memcpy(text, terminal.lexeme, terminal.length);
                text[terminal.length] = '\0';
                context->captures[i] = text;
                if (use & 1u) context->values[i] = text;
                if (use & 2u) token = text;
            }
        }
        ++child;
    }
    if (child != semantic->selection.child_count)
        goto failure;
    *value = spec->action(index, context->values, token);
    return 0;

failure:
    for (size_t i = 0; i < rule->length; ++i)
        free(context->captures[i]);
    return -1;
}

static size_t select_production(const glr_forest_node_t *node, void *opaque) {
    const parse_context *context = opaque;
    size_t best = SIZE_MAX;
    int tied = 0;
    for (size_t i = 0; i < node->child_count; ++i) {
        const int id = node->children[i]->symbol_id;
        if (id < 0 || (size_t)id >= context->spec->rule_count)
            return SIZE_MAX;
        const size_t index = context->rule_indices[id];
        if (best != SIZE_MAX) {
            const size_t old = context->rule_indices[node->children[best]->symbol_id];
            const aurocks_rule_spec *candidate = &context->spec->rules[index];
            const aurocks_rule_spec *previous = &context->spec->rules[old];
            int rank = 0;
            if (context->spec->policy == AUROCKS_PREFER &&
                candidate->preference != previous->preference)
                rank = candidate->preference > previous->preference ? 1 : -1;
            else if (candidate->priority != previous->priority)
                rank = candidate->priority > previous->priority ? 1 : -1;
            else if (candidate->preference != previous->preference)
                rank = candidate->preference > previous->preference ? 1 : -1;
            if (rank < 0) continue;
            if (rank == 0) {
                tied = 1;
                if (index >= old) continue;
            } else {
                tied = 0;
            }
        }
        best = i;
    }
    if (context->spec->policy == AUROCKS_REJECT_AMBIGUOUS && tied) return SIZE_MAX;
    return best;
}

void *aurocks_parse_glr_ex(const aurocks_parser_spec *spec, const char *input, const char **error) {
    glr_grammar_t *grammar = NULL;
    glr_parser_t *parser = NULL;
    int *symbols = NULL;
    size_t *indices = NULL;
    glr_symbol_t **body = NULL;
    size_t longest = 0, length;
    void *result = NULL;
    int has_skip = 0;
    parse_context context = {0};
    if (error) *error = input;
    if (!spec || !input || !spec->symbols || !spec->rules || !spec->action ||
        spec->start >= spec->symbol_count || spec->symbol_count > INT_MAX ||
        spec->rule_count > INT_MAX ||
        spec->symbol_count > SIZE_MAX / sizeof(*symbols) ||
        spec->rule_count > SIZE_MAX / sizeof(*indices) ||
        spec->symbols[spec->start].kind != AUROCKS_NONTERMINAL)
        return NULL;
    for (size_t i = 0; i < spec->rule_count; ++i)
        if (spec->rules[i].length > longest) longest = spec->rules[i].length;
    if (longest > SIZE_MAX / sizeof(*body) || longest > SIZE_MAX / sizeof(*context.values) ||
        longest > SIZE_MAX / sizeof(*context.captures))
        return NULL;

    length = strlen(input);
    grammar = glr_grammar_create();
    symbols = malloc(spec->symbol_count * sizeof(*symbols));
    indices = malloc(spec->rule_count * sizeof(*indices));
    body = malloc((longest ? longest : 1) * sizeof(*body));
    context.values = malloc((longest ? longest : 1) * sizeof(*context.values));
    context.captures = malloc((longest ? longest : 1) * sizeof(*context.captures));
    context.spec = spec;
    context.rule_indices = indices;
    if (!grammar || !symbols || !indices || !body || !context.values || !context.captures)
        goto done;

    for (size_t i = 0; i < spec->symbol_count; ++i) {
        const aurocks_symbol_spec *symbol = &spec->symbols[i];
        const int nonterminal = symbol->kind == AUROCKS_NONTERMINAL;
        char name[32];
        (void)snprintf(name, sizeof name, "aurocks_%zu", i);
        const int id = glr_grammar_add_symbol(grammar, nonterminal ? GLR_SYMBOL_NONTERMINAL
                                                                   : GLR_SYMBOL_TERMINAL, name);
        if (id < 0) goto done;
        symbols[i] = id;
        if (symbol->kind == AUROCKS_LITERAL &&
            glr_scannerless_set_literal(grammar, id, symbol->text, symbol->length) != 0)
            goto done;
        if (symbol->kind == AUROCKS_REGEX &&
            glr_scannerless_set_pattern(grammar, id, symbol->text, NULL, 0) != 0)
            goto done;
    }
    if (glr_grammar_set_start_symbol(grammar, symbols[spec->start]) != 0)
        goto done;
    for (size_t i = 0; i < spec->rule_count; ++i) {
        const aurocks_rule_spec *rule = &spec->rules[i];
        size_t count = 0;
        if (rule->lhs >= spec->symbol_count || (rule->length && !rule->rhs)) goto done;
        for (size_t c = 0; c < rule->length; ++c) {
            const int symbol = rule->rhs[c];
            if (symbol == -1) continue;
            if (symbol < 0 || (size_t)symbol >= spec->symbol_count) goto done;
            body[count++] = glr_grammar_get_symbol(grammar, symbols[symbol]);
        }
        const int id = glr_grammar_add_production(grammar, symbols[rule->lhs], body, count);
        if (id < 0 || (size_t)id >= spec->rule_count) goto done;
        indices[id] = i;
        if (glr_production_set_semantic_action(grammar, id, reduce, &indices[id], NULL) != 0)
            goto done;
    }
    parser = glr_parser_create(grammar);
    if (!parser || glr_parser_set_scannerless(parser, true) != 0) goto done;
    if (spec->skip) {
        if (regcomp(&context.skip, spec->skip, REG_EXTENDED) != 0) goto done;
        has_skip = 1;
        if (glr_parser_set_trivia_hook(parser, skip_trivia, &context) != 0) goto done;
    }
    glr_parser_set_user_data(parser, &context);
    glr_parser_set_semantic_resolver(parser, select_production);
    const glr_parse_result_t parsed = glr_parse(parser, input, length);
    if (parsed.error == GLR_PARSE_SUCCESS) {
        if (error) *error = NULL;
        result = parsed.semantic_value;
    } else if (error && parsed.position <= length) {
        *error = input + parsed.position;
    }

done:
    if (parser) glr_parser_destroy(parser);
    if (grammar) glr_grammar_destroy(grammar);
    if (has_skip) regfree(&context.skip);
    free(context.captures);
    free(context.values);
    free(body);
    free(indices);
    free(symbols);
    return result;
}

void *aurocks_parse_glr(const aurocks_parser_spec *spec, const char *input) {
    return aurocks_parse_glr_ex(spec, input, NULL);
}
