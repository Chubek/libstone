#include "calc.h"
#include <string.h>

int main(void) {
    const char source[] = "19 + 23\n";
    const char invalid[] = "19 +";
    Calc_Context *ctx = Calc_context_new();
    Calc_Diagnostic *diag = NULL;
    Calc_Program *program = NULL;
    double result = 0;

    if (!ctx) return 1;
    if (Calc_interpret(ctx, invalid, sizeof(invalid) - 1, &result, &diag)
        != Calc_SYNTAX_ERROR || !diag || !diag->message[0]) return 2;
    Calc_diagnostic_free(diag);
    diag = NULL;
    Calc_context_reset(ctx);
    if (Calc_interpret(ctx, source, sizeof(source) - 1, &result, &diag)
        != Calc_OK || diag || result != 42) return 3;
    if (Calc_compile(ctx, source, sizeof(source) - 1, &program, &diag)
        != Calc_OK || diag || Calc_program_size(program) < 6
        || memcmp(Calc_program_data(program), "LLSBC\1", 6)) return 4;
    Calc_program_free(program);
    Calc_context_free(ctx);
    return 0;
}
