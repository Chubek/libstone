/* Decode and verify the translated ANF Program.i with generated IR.c. */
#include "IR.h"

int main(void)
{
    struct archon_ir_program program;
    char diagnostic[256];
    int printed;

    if (archon_ir_load_embedded(&program, diagnostic, sizeof diagnostic) != ARCHON_IR_OK) {
        fprintf(stderr, "%s\n", diagnostic);
        return 1;
    }
    printed = archon_ir_print(stdout, &program);
    archon_ir_program_dispose(&program);
    return printed ? 0 : 1;
}
