#include "aurocks/Runtime.h"

#include <stdint.h>
#include <stdlib.h>

static unsigned actions;
static int nullable_values[2];

static void *action(size_t rule, void **values, char *token) {
    ++actions;
    switch (rule) {
    case 0: return (void *)((uintptr_t)values[0] + (uintptr_t)values[2]);
    case 1: {
        void *value = token && values[0] == token ? (void *)(uintptr_t)strtoul(token, NULL, 10) : NULL;
        free(token);
        return value;
    }
    case 2: return (void *)(uintptr_t)90;
    case 3: return values[0];
    case 4: return values[0] && values[1] && values[0] != values[1] ? &actions : NULL;
    case 5:
        return &nullable_values[(actions - 1) % 2];
    default: return NULL;
    }
}

static unsigned terminal_use(size_t rule, size_t position) {
    return rule == 1 && position == 0 ? 3u : 0u;
}

static void *record_rule(size_t rule, void **values, char *token) {
    (void)values;
    (void)token;
    ++actions;
    return &nullable_values[rule == 2];
}

int main(void) {
    const aurocks_symbol_spec symbols[] = {
        {AUROCKS_NONTERMINAL, "expr", 4},
        {AUROCKS_LITERAL, "+", 1},
        {AUROCKS_REGEX, "[0-9]+", 6},
        {AUROCKS_NONTERMINAL, "number", 6},
    };
    const int sum[] = {0, 1, 3}, digit[] = {2}, number[] = {3};
    aurocks_rule_spec rules[] = {
        {0, sum, 3, 0, 0}, {3, digit, 1, 0, 0}, {3, digit, 1, 0, 0}, {0, number, 1, 0, 0},
    };
    aurocks_parser_spec spec = {
        symbols, 4, rules, 4, 0, "[[:space:]]+", AUROCKS_FIRST, action, terminal_use
    };

    if ((uintptr_t)aurocks_parse_glr(&spec, " 1 +\t2 + 3 \n") != 6 || actions != 6)
        return 1;
    actions = 0;
    if (aurocks_parse_glr(&spec, "1+2?") || actions)
        return 2;
    spec.policy = AUROCKS_REJECT_AMBIGUOUS;
    if (aurocks_parse_glr(&spec, "1+2") || actions)
        return 3;
    spec.policy = AUROCKS_PRIORITY;
    rules[2].priority = 2;
    if ((uintptr_t)aurocks_parse_glr(&spec, "1+2") != 180 || actions != 4)
        return 4;

    const int plus[] = {1}, pair[] = {3, 3}, empty[] = {-1};
    const aurocks_rule_spec nullable_rules[] = {
        {0, sum, 3, 0, 0}, {3, digit, 1, 0, 0}, {3, digit, 1, 0, 0},
        {0, plus, 1, 0, 0}, {0, pair, 2, 0, 0}, {3, empty, 1, 0, 0},
    };
    spec.rules = nullable_rules;
    spec.rule_count = sizeof(nullable_rules) / sizeof(nullable_rules[0]);
    spec.policy = AUROCKS_FIRST;
    actions = 0;
    /* Only the pair consumes all of "": its two children share a forest
       node, but execute separately and return independently owned values. */
    const char *input = "";
    const char *error = input;
    if (aurocks_parse_glr_ex(&spec, input, &error) != &actions || error || actions != 3)
        return 5;

    /* Both roots accept "a ": one consumes the trailing space as part of
       the terminal, the other via %skip. Rank across both accepted spans. */
    const aurocks_symbol_spec split_symbols[] = {
        {AUROCKS_NONTERMINAL, "entry", 5}, {AUROCKS_LITERAL, "a", 1},
        {AUROCKS_LITERAL, "a ", 2},
    };
    const int short_text[] = {1}, long_text[] = {2};
    aurocks_rule_spec split_rules[] = {
        {0, short_text, 1, 0, 0}, {0, long_text, 1, 0, 0}, {0, short_text, 1, 2, 0},
    };
    spec.symbols = split_symbols;
    spec.symbol_count = sizeof(split_symbols) / sizeof(split_symbols[0]);
    spec.rules = split_rules;
    spec.rule_count = sizeof(split_rules) / sizeof(split_rules[0]);
    spec.action = record_rule;
    spec.terminal_use = NULL;
    spec.policy = AUROCKS_PRIORITY;
    actions = 0;
    if (aurocks_parse_glr(&spec, "a ") != &nullable_values[1] || actions != 1)
        return 6;
    /* Remove the duplicate short production to isolate the ambiguity
       between the two distinct root spans. */
    spec.rule_count = 2;
    spec.policy = AUROCKS_REJECT_AMBIGUOUS;
    actions = 0;
    input = "a ";
    if (aurocks_parse_glr_ex(&spec, input, &error) || error != input + 2 || actions)
        return 7;
    return 0;
}
