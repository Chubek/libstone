#include "llsynth_assembler.h"

#include <assembler.h>
#include <stdio.h>

int llsynth_assembler_flat(
    const char *source, const char *selector, const char *syntax, int use_target,
    llsynth_assembler_result *result, char *error, size_t error_size) {
    *result = (llsynth_assembler_result){0};
    if (error && error_size)
        error[0] = '\0';

    sandstone_asm_t *ctx = NULL;
    const char *selection = selector && *selector ? selector : NULL;
    int status = use_target
        ? sandstone_asm_new_target(&ctx, selection, error, error_size)
        : sandstone_asm_new(&ctx, selection, error, error_size);
    if (status) {
        if (error && error_size && !error[0])
            snprintf(error, error_size, "cannot create assembler context");
        return -1;
    }

    if (sandstone_asm_set_syntax(ctx, syntax) ||
        sandstone_asm_assemble_string(ctx, source, "<llsynth>") ||
        sandstone_asm_flat_image(ctx, &result->bytes, &result->size)) {
        if (error && error_size)
            snprintf(error, error_size, "%s", sandstone_asm_error(ctx));
        sandstone_asm_free(ctx);
        return -1;
    }

    result->owner = ctx;
    return 0;
}

void llsynth_assembler_release(llsynth_assembler_result *result) {
    sandstone_asm_free(result->owner);
    *result = (llsynth_assembler_result){0};
}
