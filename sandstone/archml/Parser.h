#ifndef SANDSTONE_ARCHML_PARSER_H
#define SANDSTONE_ARCHML_PARSER_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct archml_syntax archml_syntax;
typedef struct archml_ast_expr_node archml_syntax_node;
enum { ARCHML_SYMBOL, ARCHML_STRING, ARCHML_LIST };
archml_syntax *archml_parse_syntax(const char *, size_t *);
void archml_syntax_free(archml_syntax *);
const archml_syntax_node *archml_syntax_root(const archml_syntax *);
int archml_syntax_kind(const archml_syntax_node *);
size_t archml_syntax_offset(const archml_syntax_node *);
const char *archml_syntax_text(const archml_syntax_node *, size_t *);
size_t archml_syntax_count(const archml_syntax_node *);
const archml_syntax_node *archml_syntax_child(const archml_syntax_node *, size_t);
#ifdef __cplusplus
}
#endif
#endif
