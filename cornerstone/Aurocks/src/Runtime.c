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

/* LibGLR discards working stacks at the end of recognition. Preserve the
 * last frontier for fault handlers, but bound the total cost of copying it. */
enum { FRONTIER_LIMIT = 256, SNAPSHOT_WORK_LIMIT = 1048576 };
typedef struct fault_snapshots {
    const void *stacks[FRONTIER_LIMIT];
    size_t count, offset, work;
    int truncated;
} fault_snapshots;

static void clear_snapshots(fault_snapshots *snapshots) {
    while (snapshots->count)
        glr_stack_destroy((glr_stack_t *)snapshots->stacks[--snapshots->count]);
}

static void snapshot(glr_stack_t *stack, size_t offset, void *opaque) {
    fault_snapshots *snapshots = opaque;
    if (offset < snapshots->offset) return;
    if (offset > snapshots->offset) {
        clear_snapshots(snapshots);
        snapshots->offset = offset;
        snapshots->truncated = 0;
    }
    if (snapshots->count == FRONTIER_LIMIT || stack->height > SNAPSHOT_WORK_LIMIT - snapshots->work) {
        snapshots->truncated = 1;
        return;
    }
    snapshots->work += stack->height;
    glr_stack_t *copy = glr_stack_copy(stack);
    if (copy) snapshots->stacks[snapshots->count++] = copy;
    else snapshots->truncated = 1;
}

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

static int reduce(parse_context *context, const glr_forest_node_t *node,
                  const char *input, void **operands, void **value) {
    const aurocks_parser_spec *spec = context->spec;
    const size_t index = context->rule_indices[node->symbol_id];
    const aurocks_rule_spec *rule = &spec->rules[index];
    size_t child = 0;
    char *token = NULL;
    memset(context->captures, 0, rule->length * sizeof(*context->captures));
    for (size_t i = 0; i < rule->length; ++i) {
        const int symbol = rule->rhs[i];
        context->values[i] = NULL;
        if (symbol < 0)
            continue;
        if (spec->symbols[symbol].kind == AUROCKS_NONTERMINAL) {
            context->values[i] = operands[child];
        } else if (spec->terminal_use) {
            const unsigned use = spec->terminal_use(index, i) & 3u;
            if (use) {
                const glr_forest_node_t *terminal = node->children[child];
                const size_t length = terminal->end_position - terminal->position;
                char *text = malloc(length + 1);
                if (!text) goto failure;
                memcpy(text, input + terminal->position, length);
                text[length] = '\0';
                context->captures[i] = text;
                if (use & 1u) context->values[i] = text;
                if (use & 2u) token = text;
            }
        }
        ++child;
    }
    *value = spec->action(index, context->values, token);
    return 0;

failure:
    for (size_t i = 0; i < rule->length; ++i)
        free(context->captures[i]);
    return -1;
}

typedef struct production_choice {
    const glr_forest_node_t *node;
    size_t rule;
    int tied;
} production_choice;

static void consider(const parse_context *context, production_choice *best,
                     const glr_forest_node_t *node) {
    const size_t index = context->rule_indices[node->symbol_id];
    int rank = 1;
    if (best->node) {
        const aurocks_rule_spec *candidate = &context->spec->rules[index];
        const aurocks_rule_spec *previous = &context->spec->rules[best->rule];
        rank = 0;
        if (context->spec->policy == AUROCKS_PREFER &&
            candidate->preference != previous->preference)
            rank = candidate->preference > previous->preference ? 1 : -1;
        else if (candidate->priority != previous->priority)
            rank = candidate->priority > previous->priority ? 1 : -1;
        else if (candidate->preference != previous->preference)
            rank = candidate->preference > previous->preference ? 1 : -1;
    }
    if (rank < 0) return;
    best->tied = rank == 0;
    if (rank > 0 || index < best->rule ||
        (index == best->rule && node->end_position > best->node->end_position)) {
        best->node = node;
        best->rule = index;
    }
}

static int consider_symbol(const parse_context *context, production_choice *best,
                           const glr_forest_node_t *node) {
    if (!node || node->type != GLR_NODE_NONTERMINAL || !node->child_count) return -1;
    for (size_t i = 0; i < node->child_count; ++i) {
        const glr_forest_node_t *candidate = node->children[i];
        if (candidate->type != GLR_NODE_CONSTRUCTOR || candidate->symbol_id < 0 ||
            (size_t)candidate->symbol_id >= context->spec->rule_count)
            return -1;
        consider(context, best, candidate);
    }
    return 0;
}

static const glr_forest_node_t *select_root(const parse_context *context, const glr_parser_t *parser,
                                          int *ambiguous) {
    production_choice best = {0};
    /* Different amounts of terminal text may precede trailing %skip. LibGLR's
     * forest->root is only the last accepted root, not all accepted spans. */
    for (size_t i = 0; i < parser->stack_count; ++i) {
        const glr_stack_t *stack = parser->stacks[i];
        if (consider_symbol(context, &best,
                            glr_stack_node_get_forest_node(stack->states[stack->height - 1])) != 0)
            return NULL;
    }
    if (best.tied && context->spec->policy == AUROCKS_REJECT_AMBIGUOUS) {
        *ambiguous = 1;
        return NULL;
    }
    return best.node;
}

typedef struct visit_frame {
    const glr_forest_node_t *node;
    size_t next;
} visit_frame;

/* Like the portable runtime, bound the number of expanded occurrences. A
 * small forest can describe a much larger tree, even with no cycles. */
#define AUROCKS_TREE_LIMIT 1048576UL

static void *grow_stack(void *buffer, size_t *capacity, size_t used, size_t width) {
    if (used < *capacity) return buffer;
    if (*capacity > SIZE_MAX / 2) return NULL;
    const size_t next = *capacity ? *capacity * 2 : 16;
    if (next > SIZE_MAX / width) return NULL;
    void *grown = realloc(buffer, next * width);
    if (grown) *capacity = next;
    return grown;
}

static int evaluate(parse_context *context, const glr_forest_node_t *node,
                    const char *input, void **result) {
    const glr_forest_node_t **order = NULL;
    visit_frame *stack = NULL;
    void **values = NULL;
    size_t order_capacity = 0, count = 0, stack_capacity = 0, depth = 0, visited = 0;
    int status = -1;

    /* Resolve the whole selected tree before calling any actions. LibGLR's
     * DAG evaluator memoizes by node, but Aurocks actions own their semantic
     * values, so two occurrences of a nullable symbol need separate results. */
    while (node || depth) {
        if (node) {
            if (visited++ == AUROCKS_TREE_LIMIT) goto done;
            if (node->type == GLR_NODE_NONTERMINAL) {
                production_choice best = {0};
                if (consider_symbol(context, &best, node) != 0) goto done;
                if (best.tied && context->spec->policy == AUROCKS_REJECT_AMBIGUOUS) {
                    status = -2;
                    goto done;
                }
                node = best.node;
            }
            visit_frame *grown = grow_stack(stack, &stack_capacity, depth, sizeof(*stack));
            if (!grown) goto done;
            stack = grown;
            stack[depth++] = (visit_frame){node, 0};
            node = NULL;
        }
        visit_frame *frame = &stack[depth - 1];
        if (frame->next < frame->node->child_count) {
            node = frame->node->children[frame->next++];
        } else {
            const glr_forest_node_t **grown = grow_stack(order, &order_capacity, count, sizeof(*order));
            if (!grown) goto done;
            order = grown;
            order[count++] = frame->node;
            --depth;
        }
    }

    if (!count || count > SIZE_MAX / sizeof(*values)) goto done;
    values = malloc(count * sizeof(*values));
    if (!values) goto done;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        node = order[i];
        if (node->type == GLR_NODE_TERMINAL) {
            values[used++] = NULL;
        } else {
            void *value;
            used -= node->child_count;
            if (reduce(context, node, input, values + used, &value) != 0) goto done;
            values[used++] = value;
        }
    }
    *result = values[0];
    status = 0;

done:
    free(values);
    free(stack);
    free(order);
    return status;
}

void *aurocks_parse_glr_observed(const aurocks_parser_spec *spec, const char *input, const char **error,
                                aurocks_fault_observer observer, void *data) {
    glr_grammar_t *grammar = NULL;
    glr_parser_t *parser = NULL;
    int *symbols = NULL;
    size_t *indices = NULL;
    glr_symbol_t **body = NULL;
    size_t longest = 0, length;
    void *result = NULL;
    int has_skip = 0;
    parse_context context = {0};
    fault_snapshots snapshots;
    if (observer) {
        snapshots.count = snapshots.offset = snapshots.work = 0;
        snapshots.truncated = 0;
    }
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
    }
    parser = glr_parser_create(grammar);
    if (!parser || glr_parser_set_scannerless(parser, true) != 0) goto done;
    if (observer && glr_parser_set_snapshot_hook(parser, snapshot, &snapshots) != 0) goto done;
    if (spec->skip) {
        if (regcomp(&context.skip, spec->skip, REG_EXTENDED) != 0) goto done;
        has_skip = 1;
        if (glr_parser_set_trivia_hook(parser, skip_trivia, &context) != 0) goto done;
    }
    const glr_parse_result_t parsed = glr_parse(parser, input, length);
    aurocks_fault_frontier frontier = {0};
    if (observer)
        frontier = (aurocks_fault_frontier){snapshots.stacks, snapshots.count, snapshots.offset, snapshots.truncated};
    if (parsed.error == GLR_PARSE_SUCCESS) {
        int ambiguous = 0;
        const glr_forest_node_t *root = select_root(&context, parser, &ambiguous);
        const int evaluated = root ? evaluate(&context, root, input, &result) : ambiguous ? -2 : -1;
        if (evaluated == 0) {
            if (error) *error = NULL;
        } else {
            if (error) *error = input + length;
            if (observer && evaluated == -2)
                observer(data, AUROCKS_FAULT_LIBGLR, AUROCKS_FAULT_AMBIGUITY, parser, length, &frontier);
        }
    } else {
        const size_t offset = parsed.position <= length ? parsed.position : length;
        if (error) *error = input + offset;
        if (observer && (parsed.error == GLR_PARSE_ERROR_SYNTAX || parsed.error == GLR_PARSE_ERROR_UNRECOVERABLE))
            observer(data, AUROCKS_FAULT_LIBGLR, AUROCKS_FAULT_SYNTAX, parser, offset, &frontier);
    }

done:
    if (observer) clear_snapshots(&snapshots);
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

void *aurocks_parse_glr_ex(const aurocks_parser_spec *spec, const char *input, const char **error) {
    return aurocks_parse_glr_observed(spec, input, error, NULL, NULL);
}

void *aurocks_parse_glr(const aurocks_parser_spec *spec, const char *input) {
    return aurocks_parse_glr_ex(spec, input, NULL);
}
