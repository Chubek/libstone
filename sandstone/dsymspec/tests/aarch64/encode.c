#include "aarch64.i"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static int feature_list(const dsymspec_value *list, const char *const *expected) {
    size_t i;
    if (!list || list->kind != DSYMSPEC_VALUE_LIST)
        return 0;
    for (i = 0; expected[i]; ++i) {
        const dsymspec_value *value = dsymspec_at(list, i);
        if (!value || value->kind != DSYMSPEC_VALUE_STRING || strcmp(value->string, expected[i]))
            return 0;
    }
    return i == list->count;
}

static int profile_features(void) {
    static const char *const defaults[] = {"base", "fp", "neon", NULL};
    static const struct {
        const char *encoder;
        const char *required[5];
    } checks[] = {
        {"a64_add_x_x_x", {"base"}},
        {"a64_fadd_d_d_d", {"base", "fp"}},
        {"a64_add_v_int_v_int_v_int", {"base", "neon"}},
        {"a64_fcvt_h_d", {"base", "fp"}},
        {"a64_fadd_h_h_h", {"base", "fp", "fp16"}},
        {"a64_fadd_v_half_v_half_v_half", {"base", "fp", "neon", "fp16"}},
        {"a64_caspal_x_x_x_x_mem16", {"base", "lse"}},
        {"a64_ldapursw_x_memoff4", {"base", "rcpc", "rcpc2"}},
        {"a64_eret_", {"base", "priv"}},
        {"a64_histcnt_z_elem_p_zero_z_elem_z_elem", {"base", "sve", "sve2"}},
        {"a64_add_za_vgx2_z_group", {"base", "sme", "sme2"}},
    };
    size_t i, j;
    const dsymspec_decl *target = dsymspec_find(&dsymspec_aarch64, DSYMSPEC_DECL_TARGET, "aarch64");
    if (!target || !feature_list(dsymspec_get(target->record, "features"), defaults))
        return 0;
    for (i = 0; i < sizeof checks / sizeof checks[0]; ++i) {
        int matched = 0;
        for (j = 0; j < dsymspec_aarch64.declaration_count; ++j) {
            const dsymspec_decl *decl = &dsymspec_aarch64.declarations[j];
            const dsymspec_value *name;
            if (decl->kind != DSYMSPEC_DECL_FORM)
                continue;
            name = dsymspec_get(decl->record, "encoder");
            if (name && name->kind == DSYMSPEC_VALUE_STRING && !strcmp(name->string, checks[i].encoder)) {
                matched = feature_list(dsymspec_get(decl->record, "required"), checks[i].required);
                break;
            }
        }
        if (!matched) {
            fprintf(stderr, "form feature mismatch: %s\n", checks[i].encoder);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    const dsymspec_decl *entry;
    dsymspec_binding bindings[16] = {{0}};
    dsymspec_relocation relocations[2];
    uint8_t bytes[8];
    size_t n = 0, nr = 0;
    int i;

    if (argc < 2 || argc > 18)
        return 2;
    if (strcmp(argv[1], "@inventory") == 0) {
        size_t nforms = 0, nencoders = 0, nmnemonics = 0, j;
        if (!profile_features())
            return 1;
        for (j = 0; j < dsymspec_aarch64.declaration_count; ++j) {
            const dsymspec_decl *decl = &dsymspec_aarch64.declarations[j];
            if (decl->kind == DSYMSPEC_DECL_FORM)
                ++nforms;
            if (decl->kind == DSYMSPEC_DECL_MNEMONIC)
                ++nmnemonics;
            if (decl->kind == DSYMSPEC_DECL_ENCODER) {
                if (!decl->encoder || !decl->encoder->op_count) {
                    fprintf(stderr, "unsupported encoder: %s\n", decl->name);
                    return 1;
                }
                ++nencoders;
            }
        }
        printf("%zu forms, %zu encoders, %zu mnemonics\n", nforms, nencoders, nmnemonics);
        return 0;
    }
    entry = dsymspec_find(&dsymspec_aarch64, DSYMSPEC_DECL_ENCODER, argv[1]);
    if (!entry) {
        fprintf(stderr, "missing encoder: %s\n", argv[1]);
        return 2;
    }
    for (i = 2; i < argc; ++i) {
        char *value = strchr(argv[i], '=');
        char *end;
        if (!value || value == argv[i])
            return 2;
        *value++ = '\0';
        bindings[i - 2].name = argv[i];
        if (*value == '@') {
            bindings[i - 2].string = value + 1;
        } else {
            errno = 0;
            bindings[i - 2].integer = strtoimax(value, &end, 0);
            if (errno || *end || end == value)
                return 2;
        }
    }
    switch (dsymspec_encode(entry->encoder, bindings, (size_t)(argc - 2),
                           bytes, sizeof(bytes), &n, relocations, 2, &nr)) {
    case DSYMSPEC_ENCODE_OK:
        break;
    case DSYMSPEC_ENCODE_REJECTED:
        if (n || nr)
            return 1;
        puts("rejected");
        return 0;
    default:
        fprintf(stderr, "unexpected encode status: %s\n", argv[1]);
        return 1;
    }
    for (i = 0; i < (int)n; ++i)
        printf("%02x", (unsigned)bytes[i]);
    for (i = 0; i < (int)nr; ++i)
        printf(" %s@%s@%zu@%" PRId64, relocations[i].type_name,
               relocations[i].symbol, relocations[i].offset, relocations[i].addend);
    putchar('\n');
    return 0;
}
