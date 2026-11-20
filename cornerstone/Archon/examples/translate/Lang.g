%%
%start program
%skip /([[:space:]]+|#[^\n]*)/

program: /module\b/ name ';' functions;
functions: function functions | %empty;
function: /fn\b/ name '(' parameters ')' '->' name '{' statements '}';
parameters: parameter more_params | %empty;
more_params: ',' parameter more_params | %empty;
parameter: name ':' name;
statements: statement statements | %empty;
statement: /let\b/ name ':' name '=' expr ';'
         | /let\b/ name '=' expr ';'
         | /return\b/ expr ';';
expr: sum '==' sum | sum;
sum: sum '+' term | sum '-' term | term;
term: term '*' factor | factor;
factor: '(' expr ')' | /true\b/ | /false\b/ | /[0-9]+/ | name;
name: /[[:alpha:]_][[:alnum:]_]*/;
%%
