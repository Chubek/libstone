%{
#include <string.h>
#ifdef TEST_LINKED
#include <glr/parser.h>
#endif
static unsigned actions, faults;
static int bad_state;
%}

%start entry
%disambiguator error
%Fault "',' !','" => silent patch "" using repair
%fault "':'" => silent_patch "" using inspect
%fault "'?'" => inform "unexpected question mark" using inspect
%fault "[!?]" => warn "unexpected punctuation" using inspect
%fault "'~' / '$'" => error "explicit runtime error" using inspect
%fault "'#'" => silent_patch "#" using inspect
%fault "'%'" => silent_patch "%%" using inspect
%fault "'@'" => loud_patch ";" using invalid_patch
%fault "!." => load patch ";" "inserted semicolon" using eof_fault

entry: 'n' ';' { ++actions; $$ = NULL; }
     | 't' 'x' { ++actions; $$ = NULL; }
     | 't' 'x' { ++actions; $$ = NULL; };

%%

static void inspect(const aurocks_fault_state *s, aurocks_fault_response *response) {
    (void)response;
    ++faults;
    if (s->length != strlen(s->input) || !s->native_parser ||
        s->offset > s->match_end || s->match_end > s->length || !s->original_input ||
        s->original_offset > strlen(s->original_input)) {
        bad_state = 1;
        return;
    }
#ifdef TEST_LINKED
    if (s->backend != AUROCKS_FAULT_LIBGLR ||
        !((const glr_parser_t *)s->native_parser)->grammar ||
        s->frontier.offset > s->offset || s->frontier.truncated ||
        (s->offset != 0 && (!s->frontier.count ||
                           glr_stack_snapshot_height(s->frontier.stacks[0]) == 0)))
        bad_state = 1;
#else
    if (s->backend != AUROCKS_FAULT_EARLEY ||
        !((const aurocks_parser *)s->native_parser)->chart ||
        !((const aurocks_parser *)s->native_parser)->items)
        bad_state = 1;
#endif
}

static void repair(const aurocks_fault_state *s, aurocks_fault_response *response) {
    inspect(s, response);
    if (s->offset != 1 || s->match_end != 2 || s->input[1] != ',' || s->retry)
        bad_state = 1;
    response->replacement = ";";
}

static void invalid_patch(const aurocks_fault_state *s, aurocks_fault_response *response) {
    inspect(s, response);
    response->end = s->length + 1;
}

static void eof_fault(const aurocks_fault_state *s, aurocks_fault_response *response) {
    inspect(s, response);
    if (s->offset != s->length || s->match_end != s->offset ||
        s->reason != (s->input[0] == 't' ? AUROCKS_FAULT_AMBIGUITY : AUROCKS_FAULT_SYNTAX))
        bad_state = 1;
    if (s->reason == AUROCKS_FAULT_AMBIGUITY) {
        /* Report an attempted non-progressing patch and do not run actions. */
        response->replacement = "";
    }
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    if (yyparse(argv[1]) != NULL || bad_state) return 2;
    const char *error = yyparse_error();
    if (error) printf("%td ", error - argv[1]);
    else printf("ok ");
    printf("%u %u\n", actions, faults);
    return 0;
}
