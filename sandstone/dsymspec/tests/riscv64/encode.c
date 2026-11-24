#include "riscv64.i"
#include "riscv32.i"
#include "rv64im.i"

#include <stdio.h>
#include <stdlib.h>

static void list_forms(const dsymspec_target *target) {
    size_t i, j;
    for (i = 0; i < target->declaration_count; ++i) {
        const dsymspec_decl *decl = target->declarations + i;
        const dsymspec_value *fields, *value;
        if (decl->kind == DSYMSPEC_DECL_TARGET) {
            fields = dsymspec_get(decl->record, "features");
            printf("@%s\t", dsymspec_get(decl->record, "subarch")->string);
            for (j = 0; (value = dsymspec_at(fields, j)); ++j)
                printf("%s%s", j ? "," : "", value->string);
            putchar('\n');
        }
        if (decl->kind != DSYMSPEC_DECL_FORM) continue;
        printf("%s\t%s\t", decl->name, dsymspec_get(decl->record, "encoder")->string);
        fields = dsymspec_get(decl->record, "required");
        for (j = 0; (value = dsymspec_at(fields, j)); ++j)
            printf("%s%s", j ? "," : "", value->string);
        putchar('\t');
        fields = dsymspec_get(decl->record, "operands");
        for (j = 0; (value = dsymspec_at(fields, j)); ++j) {
            const dsymspec_value *kind = dsymspec_get(value, "kind");
            const dsymspec_value *cls = dsymspec_get(value, "class");
            const dsymspec_value *minimum = dsymspec_get(value, "imm_min");
            const dsymspec_value *maximum = dsymspec_get(value, "imm_max");
            printf("%s%s", j ? "," : "", cls ? cls->string : kind->string);
            if (minimum && maximum)
                printf(":%lld:%lld", (long long)minimum->integer, (long long)maximum->integer);
        }
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    const dsymspec_target *target;
    char line[256];
    if (argc != 3) return 2;
    if (!strcmp(argv[1], "riscv64")) target = &dsymspec_riscv64;
    else if (!strcmp(argv[1], "riscv32")) target = &dsymspec_riscv32;
    else if (!strcmp(argv[1], "rv64im")) target = &dsymspec_rv64im;
    else return 2;
    if (!strcmp(argv[2], "list")) {
        list_forms(target);
        return 0;
    }
    if (strcmp(argv[2], "encode")) return 2;
    while (fgets(line, sizeof(line), stdin)) {
        char *token = strtok(line, " \n");
        dsymspec_binding bindings[8];
        dsymspec_relocation relocs[2];
        uint8_t bytes[32];
        size_t n = 0, nr = 0, size = 0, i;
        dsymspec_encode_status status;
        const dsymspec_decl *decl = dsymspec_find(target, DSYMSPEC_DECL_ENCODER, token);
        if (!decl) return 3;
        while ((token = strtok(NULL, " \n"))) {
            char *value = strchr(token, '=');
            if (!value || n == sizeof(bindings) / sizeof(*bindings)) return 2;
            *value++ = '\0';
            bindings[n].name = token;
            bindings[n].integer = value[0] == '@' ? 0 : strtoll(value, NULL, 0);
            bindings[n].string = value[0] == '@' ? value + 1 : NULL;
            ++n;
        }
        status = dsymspec_encode(decl->encoder, bindings, n, bytes, sizeof(bytes), &size,
                                 relocs, sizeof(relocs) / sizeof(*relocs), &nr);
        if (status == DSYMSPEC_ENCODE_REJECTED) {
            puts("rejected");
            continue;
        }
        if (status != DSYMSPEC_ENCODE_OK) {
            printf("status:%d\n", status);
            continue;
        }
        for (i = 0; i < size; ++i) printf("%02x", bytes[i]);
        for (i = 0; i < nr; ++i)
            printf("|%s:%s:%zu:%lld", relocs[i].type_name, relocs[i].symbol,
                   relocs[i].offset, (long long)relocs[i].addend);
        putchar('\n');
    }
    return ferror(stdin) ? 2 : 0;
}
