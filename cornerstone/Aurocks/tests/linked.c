#include "aurocks/Runtime.h"

#include <stdint.h>
#include <stdlib.h>

static unsigned actions;

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
    default: return NULL;
    }
}

static unsigned terminal_use(size_t rule, size_t position) {
    return rule == 1 && position == 0 ? 3u : 0u;
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
    return 0;
}
