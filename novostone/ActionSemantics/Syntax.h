#ifndef NOVOSTONE_ACTION_SEMANTICS_SYNTAX_H
#define NOVOSTONE_ACTION_SEMANTICS_SYNTAX_H
#ifdef __cplusplus
extern "C" {
#endif
#include "ActionSemanticsAST.h"
typedef struct novo_as_syntax novo_as_syntax;
novo_as_syntax *novo_as_parse_syntax(const char *input, size_t *error);
novo_as_document_t novo_as_syntax_document(const novo_as_syntax *syntax);
void novo_as_syntax_free(novo_as_syntax *syntax);
#ifdef __cplusplus
}
#endif
#endif
