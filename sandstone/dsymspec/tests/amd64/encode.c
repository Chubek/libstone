#include "amd64.i"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run the generated C11 encoder, accepting one test per input line:
   encoder binding=value binding=@symbol ...
   Output bytes and relocation records only on success. */
int main(void) {
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        const char *name = strtok(line, " \n");
        const dsymspec_decl *decl;
        dsymspec_binding args[24];
        dsymspec_relocation rel;
        uint8_t out[15];
        size_t count = 0, size = 0, relocs = 0, i;
        char *arg;
        memset(out, 0xa5, sizeof out);
        if (!name) return 2;
        decl = dsymspec_find(&dsymspec_amd64, DSYMSPEC_DECL_ENCODER, name);
        if (!decl) {
            puts("missing");
            continue;
        }
        while ((arg = strtok(NULL, " \n"))) {
            char *value = strchr(arg, '=');
            if (!value || count == sizeof args / sizeof *args) return 2;
            *value++ = '\0';
            args[count].name = arg;
            args[count].string = *value == '@' ? value + 1 : NULL;
            args[count++].integer = *value == '@' ? 0 : strtoimax(value, NULL, 0);
        }
        switch (dsymspec_encode(decl->encoder, args, count, out, sizeof out, &size,
                                &rel, 1, &relocs)) {
        case DSYMSPEC_ENCODE_OK:
            for (i = 0; i < size; ++i) printf("%02x", (unsigned)out[i]);
            if (relocs) {
                printf(" %zu:%s:%s:%" PRId64, rel.offset, rel.type_name, rel.symbol, rel.addend);
            }
            puts("");
            break;
        case DSYMSPEC_ENCODE_REJECTED:
            if (size != 0 || relocs != 0) return 2;
            for (i = 0; i < sizeof out; ++i) if (out[i] != 0xa5) return 2;
            puts("rejected");
            break;
        default:
            puts("encoder error");
            break;
        }
    }
    return 0;
}
