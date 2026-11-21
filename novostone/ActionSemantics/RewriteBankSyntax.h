#ifndef NOVOSTONE_REWRITE_BANK_SYNTAX_H
#define NOVOSTONE_REWRITE_BANK_SYNTAX_H
#ifdef __cplusplus
extern "C" {
#endif
#include "RewriteBanksAST.h"
typedef struct novo_rwb_syntax novo_rwb_syntax;
novo_rwb_syntax *novo_rwb_parse_syntax(const char *, size_t *);
novo_rwb_document_t novo_rwb_syntax_document(const novo_rwb_syntax *);
void novo_rwb_syntax_free(novo_rwb_syntax *);
#ifdef __cplusplus
}
#endif
#endif
