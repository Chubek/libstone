#include <sandstone/sdk.h>
#include <stdio.h>
#include <string.h>

static int value(void *user, const char *args, sandstone_source_sink emit,
                 void *sink, char *error, size_t error_size) {
    (void)user; (void)error; (void)error_size;
    return emit(sink, ".byte ", 6) || emit(sink, args, strlen(args)) || emit(sink, "\n", 1);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2; /* path to installed amd64.aml */
    sandstone_asm_t *as = NULL;
    char error[1024] = {0};
    if (sandstone_asm_new_archml(&as, argv[1], error, sizeof error)) {
        fprintf(stderr, "%s\n", error); return 1;
    }
    int failed = sandstone_asm_register_directive(as, ".value", value, NULL) ||
        sandstone_asm_define_macro(as, "pair", "n = 7", ".value \\n\n.value \\n") ||
        sandstone_asm_assemble_string(as,
            ".insn nop\npair\n<: lsandas.injectf('.byte ~li', {1,2}) :>\n", "example.s");
    size_t size = 0;
    const unsigned char *image = sandstone_asm_image(as, &size);
    const unsigned char expected[] = {0x90, 7, 7, 1, 2};
    if (!failed) failed = size != sizeof expected || memcmp(image, expected, sizeof expected);
    if (failed) fprintf(stderr, "%s\n", sandstone_asm_error(as));
    sandstone_asm_free(as);
    return failed ? 1 : 0;
}
