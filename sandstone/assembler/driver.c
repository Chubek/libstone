/* sandstone-as: SandAS command line. */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "intern.h"
#include "sdk.h"

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [options] FILE...\n"
            "\n"
            "  --target PROFILE    built-in DSymspec target (default: build target)\n"
            "  -b, --bank BANK     use a legacy .symas databank instead\n"
            "  --archml PROFILE   ArchML .insn encoder (also usable standalone)\n"
            "  -s, --syntax NAME   syntax record (default: first in target)\n"
            "  -f, --format FMT    bin (flat image, default), obj (JSON), elf (ET_REL),\n"
            "                      coff (Windows .obj), macho (Darwin MH_OBJECT)\n"
            "  -F, --feature NAME  enable feature (repeatable); prefix with\n"
            "                      '-' to disable (e.g. -F -avx)\n"
            "  --lua HOOK.lua      source preprocessing hook\n"
            "  --asm-opt FILE.lua  run asm_optimize after source expansion\n"
            "  -o, --output OUT    output path (default: out.bin / out.obj / out.o)\n"
            "  --list-syntaxes     list syntax records in the target and exit\n"
            "  -h, --help          print this text\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *bank = NULL;
    const char *profile = NULL;
    const char *archml = NULL;
    const char *syntax = NULL;
    const char *format = "bin";
    const char *output = NULL;
    const char *hook = NULL;
    const char *asm_opt = NULL;
    int list_syntaxes = 0;
    char *features[256];
    size_t n_features = 0;

    static const struct option longs[] = {
        {"bank", required_argument, NULL, 'b'},
        {"target", required_argument, NULL, 't'},
        {"archml", required_argument, NULL, 'A'},
        {"syntax", required_argument, NULL, 's'},
        {"format", required_argument, NULL, 'f'},
        {"feature", required_argument, NULL, 'F'},
        {"lua", required_argument, NULL, 'L'},
        {"asm-opt", required_argument, NULL, 'O'},
        {"output", required_argument, NULL, 'o'},
        {"list-syntaxes", no_argument, NULL, 'l'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "b:s:f:F:o:h", longs, NULL)) != -1) {
        switch (c) {
        case 'b': bank = optarg; break;
        case 't': profile = optarg; break;
        case 'A': archml = optarg; break;
        case 's': syntax = optarg; break;
        case 'f': format = optarg; break;
        case 'F':
            if (n_features < 256)
                features[n_features++] = optarg;
            break;
        case 'L': hook = optarg; break;
        case 'O': asm_opt = optarg; break;
        case 'o': output = optarg; break;
        case 'l': list_syntaxes = 1; break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (bank && profile) {
        fprintf(stderr, "sandstone-as: --bank and --target are mutually exclusive\n");
        return 2;
    }
    if (strcmp(format, "bin") && strcmp(format, "obj") && strcmp(format, "elf") &&
        strcmp(format, "coff") && strcmp(format, "macho")) {
        fprintf(stderr, "sandstone-as: unknown format '%s'\n", format);
        return 2;
    }

    sandstone_asm_t *ctx = NULL;
    char err[1024] = {0};
    int load = bank ? sandstone_asm_new(&ctx, bank, err, sizeof err) :
        archml && !profile ? sandstone_asm_new_archml(&ctx, archml, err, sizeof err) :
        profile ? sandstone_asm_new_target(&ctx, profile, err, sizeof err) :
                  sandstone_asm_new(&ctx, NULL, err, sizeof err);
    if (load != 0) {
        fprintf(stderr, "sandstone-as: %s\n", err[0] ? err : "cannot initialize assembler");
        return 1;
    }
    if (ctx->bank && archml && sandstone_asm_load_archml(ctx, archml) != 0) {
        fprintf(stderr, "sandstone-as: %s\n", sandstone_asm_error(ctx));
        sandstone_asm_free(ctx);
        return 1;
    }
    if (list_syntaxes) {
        if (!ctx->bank) {
            fprintf(stderr, "sandstone-as: --list-syntaxes requires a built-in target or bank\n");
            sandstone_asm_free(ctx);
            return 1;
        }
        for (uint32_t i = 0; i < domsymas_syntax_count(ctx->bank); i++) {
            const DomSymAs_Syntax *s =
                domsymas_syntax_at(ctx->bank, i);
            printf("%s\n", s && s->name ? s->name : "(unnamed)");
        }
        sandstone_asm_free(ctx);
        return 0;
    }
    if (syntax && sandstone_asm_set_syntax(ctx, syntax) != 0) {
        fprintf(stderr, "sandstone-as: %s\n", sandstone_asm_error(ctx));
        sandstone_asm_free(ctx);
        return 1;
    }
    for (size_t i = 0; i < n_features; i++) {
        int rc;
        if (features[i][0] == '-' && features[i][1])
            rc = sandstone_asm_disable_feature(ctx, features[i] + 1);
        else
            rc = sandstone_asm_enable_feature(ctx, features[i]);
        if (rc != 0) {
            fprintf(stderr, "sandstone-as: %s\n", sandstone_asm_error(ctx));
            sandstone_asm_free(ctx);
            return 1;
        }
    }
    if (hook && sandstone_asm_load_hook(ctx, hook) != 0) {
        fprintf(stderr, "sandstone-as: %s\n", sandstone_asm_error(ctx));
        sandstone_asm_free(ctx);
        return 1;
    }
    if (asm_opt && sandstone_asm_load_asm_opt_hook(ctx, asm_opt) != 0) {
        fprintf(stderr, "sandstone-as: %s\n", sandstone_asm_error(ctx));
        sandstone_asm_free(ctx);
        return 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "sandstone-as: no input files\n");
        usage(argv[0]);
        sandstone_asm_free(ctx);
        return 2;
    }
    /* Concatenate inputs so one build may span several files. */
    size_t cap = 1 << 16, len = 0;
    char *src = malloc(cap);
    if (!src) {
        sandstone_asm_free(ctx);
        return 1;
    }
    src[0] = '\0';
    for (int i = optind; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            fprintf(stderr, "sandstone-as: cannot open '%s'\n", argv[i]);
            free(src);
            sandstone_asm_free(ctx);
            return 1;
        }
        char chunk[8192];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
            while (len + n + 2 > cap) {
                cap *= 2;
                char *nsrc = realloc(src, cap);
                if (!nsrc) {
                    fclose(f);
                    free(src);
                    sandstone_asm_free(ctx);
                    return 1;
                }
                src = nsrc;
            }
            memcpy(src + len, chunk, n);
            len += n;
        }
        fclose(f);
        if (len + 2 > cap) {
            cap *= 2;
            char *nsrc = realloc(src, cap);
            if (!nsrc) {
                free(src);
                sandstone_asm_free(ctx);
                return 1;
            }
            src = nsrc;
        }
        src[len++] = '\n';
        src[len] = '\0';
    }
    /* Keep the first input's path as the include base even when multiple
       source files are concatenated into one assembly unit. */
    if (!strcmp(format, "elf") || !strcmp(format, "coff") || !strcmp(format, "macho")) {
        if (!output) output = !strcmp(format, "coff") ? "out.obj" : "out.o";
        int rc = sandstone_asm_assemble_object(ctx, src, argv[optind], output, format);
        if (rc) fprintf(stderr, "sandstone-as: %s\n", sandstone_asm_error(ctx));
        free(src);
        sandstone_asm_free(ctx);
        return rc != 0;
    }
    if (sandstone_asm_assemble_string(ctx, src, argv[optind]) != 0) {
        fprintf(stderr, "sandstone-as: %s\n", sandstone_asm_error(ctx));
        free(src);
        sandstone_asm_free(ctx);
        return 1;
    }
    free(src);
    char default_out[16];
    if (!output) {
        snprintf(default_out, sizeof default_out, "out.%s", format);
        output = default_out;
    }
    if (sandstone_asm_write(ctx, output, format) != 0) {
        fprintf(stderr, "sandstone-as: %s\n", sandstone_asm_error(ctx));
        sandstone_asm_free(ctx);
        return 1;
    }
    uint32_t nsec = sandstone_asm_section_count(ctx);
    size_t total = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        size_t l = 0;
        sandstone_asm_section_bytes(ctx, i, &l);
        total += l;
    }
    uint32_t nrel = 0, nsym = 0;
    sandstone_asm_relocs(ctx, &nrel);
    sandstone_asm_symbols(ctx, &nsym);
    printf("wrote %s (%zu bytes, %u section(s), %u symbol(s), %u "
           "reloc(s))\n",
           output, total, nsec, nsym, nrel);
    sandstone_asm_free(ctx);
    return 0;
}
