%{
#include <stdint.h>
#include <stdlib.h>
%}
%start expr
%skip /[ \t\r\n]+/
%%
expr: expr '+' number { $$ = (void *)((uintptr_t)$1 + (uintptr_t)$3); }
    | number          { $$ = $1; }
    ;
number: /\d+/ {
    if ($1 == $TOKEN) $$ = (void *)(uintptr_t)strtoul($TOKEN, NULL, 10);
    aurocks_free($TOKEN);
};
%%
int main(void) {
    if ((uintptr_t)parse_linked("  21 +  7 +\t14 \n") != 42) return 1;
    if ((uintptr_t)parse_expr("20+22") != 42) return 2;
    if ((uintptr_t)yyparse("42") != 42) return 3;
    const char *invalid = "42?";
    if (parse_linked(invalid) != NULL || parse_linked_error() != invalid + 2) return 4;
    if (parse_expr_error() != parse_linked_error()) return 5;
    return 0;
}
