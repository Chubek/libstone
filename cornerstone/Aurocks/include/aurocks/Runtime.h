#ifndef AUROCKS_RUNTIME_H
#define AUROCKS_RUNTIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C data interface used by --backend libglr generated parsers. */
typedef enum aurocks_symbol_kind {
    AUROCKS_NONTERMINAL, AUROCKS_LITERAL, AUROCKS_REGEX
} aurocks_symbol_kind;

typedef struct aurocks_symbol_spec {
    aurocks_symbol_kind kind;
    const char *text;
    size_t length;
} aurocks_symbol_spec;

typedef struct aurocks_rule_spec {
    size_t lhs;
    const int *rhs;  /* Symbol indices; -1 preserves an explicit %empty slot. */
    size_t length;
    long priority;
    int preference;
} aurocks_rule_spec;

typedef enum aurocks_forest_policy {
    AUROCKS_FIRST, AUROCKS_REJECT_AMBIGUOUS, AUROCKS_PRIORITY, AUROCKS_PREFER
} aurocks_forest_policy;

/* Captured terminal strings are heap-owned and passed to the action, which
 * can retain or free them. A terminal used by both selectors shares one copy. */
typedef void *(*aurocks_action_fn)(size_t rule, void **values, char *token);
/* Return bit 0 for a positional reference and bit 1 for $TOKEN. */
typedef unsigned (*aurocks_terminal_use_fn)(size_t rule, size_t position);

typedef struct aurocks_parser_spec {
    const aurocks_symbol_spec *symbols;
    size_t symbol_count;
    const aurocks_rule_spec *rules;
    size_t rule_count;
    size_t start;
    const char *skip;
    aurocks_forest_policy policy;
    aurocks_action_fn action;
    aurocks_terminal_use_fn terminal_use;
} aurocks_parser_spec;

void *aurocks_parse_glr(const aurocks_parser_spec *spec, const char *input);
/* On failure, error points into input at the furthest reported position.
 * A successful action may return NULL without setting an error. */
void *aurocks_parse_glr_ex(const aurocks_parser_spec *spec, const char *input, const char **error);

#ifdef __cplusplus
}
#endif

#endif
