#ifndef NOVOSTONE_DATALOG_SYNTAX_H
#define NOVOSTONE_DATALOG_SYNTAX_H
#ifdef __cplusplus
extern "C" {
#endif
#include "DatalogAST.h"
typedef struct novo_dl_syntax novo_dl_syntax;
/* Internal API. The C++ entry point serializes access to Aurocks globals. */
novo_dl_syntax *novo_dl_parse_syntax(const char *input, size_t *error);
novo_dl_document_t novo_dl_syntax_document(const novo_dl_syntax *syntax);
void novo_dl_syntax_free(novo_dl_syntax *syntax);
#ifdef __cplusplus
}
#endif
#endif
