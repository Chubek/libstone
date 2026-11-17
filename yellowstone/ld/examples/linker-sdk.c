#include <qobjld/ld.h>
#include <stdio.h>

/* Generate a freestanding Linux x86-64 executable that exits with status 7. */
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    qld_linker_t *ld = qld_create();
    if (!ld) return 1;
    char *error = NULL;
    const char *parameters[] = {"status"};
    int failed = qld_define_macro(ld, "exit_program", parameters, 1,
        "ENTRY(_start)\nSECTIONS { .text : { _start = .; "
        "BYTE(0xb8) LONG(60) BYTE(0xbf) LONG(${status}) BYTE(0x0f) BYTE(0x05) } }", &error);
    if (!failed) {
        qld_set_output(ld, argv[1]);
        qld_set_script_text(ld, "@exit_program 7\n");
        failed = qld_run(ld, &error);
    }
    if (failed) fprintf(stderr, "%s\n", error ? error : "link failed");
    qld_free_string(error);
    qld_destroy(ld);
    return failed ? 1 : 0;
}
