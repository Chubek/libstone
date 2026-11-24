%{
#include <stdlib.h>

static int numbers[32];
static unsigned used;

static void *save_number(int n)
{
    numbers[used] = n;
    return &numbers[used++];
}
%}

%start expr
%skip /[[:space:]]+/

expr:
      expr '+' term { $$ = save_number(*(int *)$1 + *(int *)$3); }
    | expr '-' term { $$ = save_number(*(int *)$1 - *(int *)$3); }
    | term          { $$ = $1; }
    ;

term:
      term '*' atom { $$ = save_number(*(int *)$1 * *(int *)$3); }
    | atom          { $$ = $1; }
    ;

atom:
      '(' expr ')' { $$ = $2; }
    | /[0-9]+/     { int n = atoi($TOKEN); free($TOKEN); $$ = save_number(n); }
    ;

%%

int main(void)
{
    int *answer = yyparse(" 18 - 3 - 2 *\t(4 + 1)\n");
    if (!answer || *answer != 5)
        return 1;
    if (yyparse("1 +") || yyparse("1 ? 2"))
        return 2;
    return 0;
}
