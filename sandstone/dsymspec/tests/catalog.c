#include "DSymspec.h"
#include "dsymspec_test_catalog.inc"
/* Both the common API and each generated profile must be safe to re-include. */
#include "rv64im.i"
#include "DSymspec.h"
#include <stdio.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static const dsymspec_target *const profiles[] = {
#include "dsymspec_test_profiles.inc"
};

#define COUNT_DECLARATION(kind, index, name, record) + 1
enum { RV64IM_COUNT = 0 DSYMSPEC_RV64IM_DECLARATIONS(COUNT_DECLARATION) };
#undef COUNT_DECLARATION

struct vector {
    const dsymspec_target *target;
    const char *encoder, *asm_text, *bytes;
    size_t size;
    dsymspec_binding bindings[5]; /* More than any test vector; zero-filled terminator. */
};

#define OP(name, value) { name, value, NULL }
#define OPERANDS(...) { __VA_ARGS__, OP(NULL, 0) }
#define NO_OPERANDS { OP(NULL, 0) }
#define ENCODING(profile, encoder, triple, cpu, asm_text, bytes, inputs) \
    { &dsymspec_##profile, encoder, asm_text, bytes, sizeof(bytes) - 1, inputs },
static const struct vector vectors[] = {
#include "encodings.def"
};
#undef ENCODING
#undef NO_OPERANDS
#undef OPERANDS
#undef OP

int main(void) {
    size_t i;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        const dsymspec_target *target = profiles[i];
        size_t j;
        size_t encoders = 0;
        int have_arch = 0;
        CHECK(target->declaration_count > 0);
        for (j = 0; j < target->declaration_count; ++j) {
            const dsymspec_decl *decl = &target->declarations[j];
            if (decl->kind == DSYMSPEC_DECL_TARGET) {
                const dsymspec_value *arch = dsymspec_get(decl->record, "arch");
                CHECK(arch && arch->kind == DSYMSPEC_VALUE_STRING && arch->count > 0);
                have_arch = 1;
            } else if (decl->kind == DSYMSPEC_DECL_ENCODER) {
                CHECK(decl->encoder);
                if (decl->encoder->unsupported_reason) {
                    fprintf(stderr, "%s/%s: %s\n", target->name, decl->name,
                            decl->encoder->unsupported_reason);
                    return 1;
                }
                ++encoders;
            }
        }
        CHECK(have_arch);
        if (target == &dsymspec_directx || target == &dsymspec_nvptx)
            CHECK(encoders == 0);
        else
            CHECK(encoders > 0);
    }
    CHECK(RV64IM_COUNT == DSYMSPEC_RV64IM_DECLARATION_COUNT);
    {
        const dsymspec_decl *target = dsymspec_find(&dsymspec_rv64im, DSYMSPEC_DECL_TARGET, "riscv64");
        const dsymspec_value *features, *item;
        CHECK(target);
        features = dsymspec_get(target->record, "features");
        CHECK(features && features->kind == DSYMSPEC_VALUE_LIST && features->count == 2);
        item = dsymspec_at(features, 1);
        CHECK(item && item->kind == DSYMSPEC_VALUE_STRING && !strcmp(item->string, "m"));
    }
    for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        const struct vector *v = &vectors[i];
        const dsymspec_decl *decl = dsymspec_find(v->target, DSYMSPEC_DECL_ENCODER, v->encoder);
        size_t size = 0, count = 0;
        uint8_t bytes[32] = {0};
        dsymspec_encode_status status;
        while (v->bindings[count].name) ++count;
        if (!decl) {
            fprintf(stderr, "%s: missing encoder %s\n", v->target->name, v->encoder);
            return 1;
        }
        status = dsymspec_encode(decl->encoder, v->bindings, count, bytes, sizeof(bytes),
                                 &size, NULL, 0, NULL);
        if (status != DSYMSPEC_ENCODE_OK || size != v->size || memcmp(bytes, v->bytes, v->size)) {
            fprintf(stderr, "%s/%s: %s: status %d, emitted", v->target->name, v->encoder,
                    v->asm_text, (int)status);
            for (size_t j = 0; j < size; ++j) fprintf(stderr, " %02x", (unsigned)bytes[j]);
            fputc('\n', stderr);
            return 1;
        }
    }
    {
        const dsymspec_decl *decl = dsymspec_find(&dsymspec_amd64, DSYMSPEC_DECL_ENCODER, "call");
        dsymspec_binding input = {"sym", 0, "external"};
        dsymspec_relocation reloc;
        uint8_t bytes[5] = {42, 42, 42, 42, 42};
        size_t size, relocs;
        CHECK(decl);
        CHECK(dsymspec_encode(decl->encoder, &input, 1, bytes, 4, &size, &reloc, 1, &relocs)
              == DSYMSPEC_ENCODE_CAPACITY);
        CHECK(size == 0 && relocs == 0 && bytes[0] == 42 && bytes[4] == 42);
        CHECK(dsymspec_encode(decl->encoder, &input, 1, bytes, sizeof(bytes), &size, &reloc, 1, &relocs)
              == DSYMSPEC_ENCODE_OK);
        CHECK(size == 5 && !memcmp(bytes, "\xe8\0\0\0\0", 5));
        CHECK(relocs == 1 && reloc.offset == 1 && reloc.addend == -4);
        CHECK(!strcmp(reloc.symbol, "external"));
        CHECK(reloc.type_name && !strcmp(reloc.type_name, "R_X86_64_PLT32"));
        decl = dsymspec_find(&dsymspec_x86, DSYMSPEC_DECL_ENCODER, "call");
        CHECK(decl);
        CHECK(dsymspec_encode(decl->encoder, &input, 1, bytes, sizeof(bytes), &size, &reloc, 1, &relocs)
              == DSYMSPEC_ENCODE_OK);
        CHECK(size == 5 && !memcmp(bytes, "\xe8\xfc\xff\xff\xff", 5));
        CHECK(relocs == 1 && reloc.offset == 1 && reloc.addend == -4);
        CHECK(reloc.type_name && !strcmp(reloc.type_name, "R_386_PC32"));
    }
    {
        const dsymspec_decl *decl = dsymspec_find(&dsymspec_wasm, DSYMSPEC_DECL_ENCODER,
                                                  "i32.const_imm");
        dsymspec_binding input = {"a", -INT64_C(2147483649), NULL};
        uint8_t bytes[6] = {42};
        size_t size;
        CHECK(decl);
        CHECK(dsymspec_encode(decl->encoder, NULL, 0, bytes, sizeof(bytes), &size, NULL, 0, NULL)
              == DSYMSPEC_ENCODE_MISSING_BINDING);
        CHECK(dsymspec_encode(decl->encoder, &input, 1, bytes, sizeof(bytes), &size, NULL, 0, NULL)
              == DSYMSPEC_ENCODE_REJECTED);
        CHECK(size == 0 && bytes[0] == 42);
    }
    return 0;
}
