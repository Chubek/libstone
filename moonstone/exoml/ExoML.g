%{
#include <stdio.h>
%}
%skip /[ \t\r\n]+/
%%
program: statement* ;
statement: /[A-Za-z_][A-Za-z0-9_]*/ '~setconst!' call ';'
         | /[A-Za-z_][A-Za-z0-9_]*/ '~setvar!' call ';'
         | /[A-Za-z_][A-Za-z0-9_]*/ '~get!' call ';'
         | /[A-Za-z_][A-Za-z0-9_]*/ '~hatch!' call ';'
         ;
call: /[A-Za-z_][A-Za-z0-9_:]*/ '(' arguments? ')' ;
arguments: argument (',' argument)* ;
argument: /[A-Za-z_][A-Za-z0-9_]*/ | /[0-9]+/ | /"[^"]*"/ ;
%%
