%{
#include <stdlib.h>

static int object_value, array_value, string_value, true_value, false_value, null_value;
static double number_value;
%}

%start json
%skip /[[:space:]]+/

json:
      object { $$ = $1; }
    | array  { $$ = $1; }
    | string { $$ = $1; }
    | number { $$ = $1; }
    | 'true' { $$ = &true_value; }
    | 'false' { $$ = &false_value; }
    | 'null' { $$ = &null_value; }
    ;

object: '{' members '}' { $$ = &object_value; };
members: member tail_members | %empty;
member: string ':' json;
tail_members: ',' member tail_members | %empty;

array: '[' elements ']' { $$ = &array_value; };
elements: json tail_elements | %empty;
tail_elements: ',' json tail_elements | %empty;

/* A small JSON fixture, not a complete JSON implementation. */
string: /"[^"\\]*"/ { free($TOKEN); $$ = &string_value; };
number: /-?(0|[1-9][0-9]*)(\.[0-9]+)?/ {
    if (*$TOKEN) {
        number_value = strtod($TOKEN, NULL);
        $$ = &number_value;
    }
    free($TOKEN);
};

%%

int main(void)
{
    if (parse_json(" {\"values\": [1, 2.5, true, false, null], \"s\": \"x\"} ") != &object_value)
        return 1;
    if (parse_json("[]") != &array_value || parse_json("true") != &true_value)
        return 2;
    if (parse_json("-2.5") != &number_value || number_value != -2.5)
        return 3;
    if (parse_json("{\"x\": [true,]}") || parse_json(" null junk"))
        return 4;
    return 0;
}
