#include "Backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv)
{
    ArchonBackendArtifact artifact = {0};
    ArchonBackendDiagnostic diag;
    FILE *input = stdin;
    size_t size;
    char *buffer;
    unsigned flags = 0;
    int verify = 0, read_error, rc = 1, i;
    if (argc == 2 && !strcmp(argv[1], "--list-capabilities")) {
        printf("%s: canonical-mir-v1 -> GAS assembly, physical-v1 records, or raw machine code "
               "(one function, no imports or globals)\n",
            archon_backend_target());
        return ferror(stdout) ? 1 : 0;
    }
    if (argc < 3 || (strcmp(argv[1], "--input") && strcmp(argv[1], "--verify-input"))) {
        fputs("usage: archon-backend --input PATH|- [--emit-physical|--emit-code] [--schedule-pure] > OUT\n"
              "       archon-backend --verify-input PATH|-\n"
              "       archon-backend --list-capabilities\n", stderr);
        return 2;
    }
    verify = strcmp(argv[1], "--verify-input") == 0;
    for (i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--emit-physical") && !verify)
            flags |= ARCHON_BACKEND_PHYSICAL_RECORDS;
        else if (!strcmp(argv[i], "--emit-code") && !verify)
            flags |= ARCHON_BACKEND_MACHINE_CODE;
        else if (!strcmp(argv[i], "--schedule-pure") && !verify)
            flags |= ARCHON_BACKEND_SCHEDULE_PURE;
        else {
            fprintf(stderr, "archon-backend: unsupported option %s\n", argv[i]);
            return 2;
        }
    }
    if ((flags & ARCHON_BACKEND_PHYSICAL_RECORDS) && (flags & ARCHON_BACKEND_MACHINE_CODE)) {
        fputs("archon-backend: choose one output mode\n", stderr);
        return 2;
    }
#ifdef _WIN32
    if ((flags & ARCHON_BACKEND_MACHINE_CODE) && _setmode(_fileno(stdout), _O_BINARY) == -1) {
        perror("archon-backend: binary output");
        return 1;
    }
#endif
    if (strcmp(argv[2], "-") && !(input = fopen(argv[2], "rb"))) {
        perror(argv[2]);
        return 1;
    }
    buffer = malloc(ARCHON_MIR_TEXT_MAX_BYTES + 1u);
    if (!buffer) {
        if (input != stdin)
            (void)fclose(input);
        fputs("archon-backend: out of memory\n", stderr);
        return 1;
    }
    size = fread(buffer, 1, ARCHON_MIR_TEXT_MAX_BYTES + 1u, input);
    read_error = ferror(input);
    if (input != stdin && fclose(input))
        read_error = 1;
    if (read_error)
        fputs("archon-backend: input read failed\n", stderr);
    else if (archon_backend_compile_text_ex(buffer, size, NULL, flags, &artifact, &diag) != ARCHON_BACKEND_OK) {
        fprintf(stderr, "archon-backend: stage %d", (int)diag.stage);
        if (diag.function != SIZE_MAX)
            fprintf(stderr, ", function %zu", diag.function);
        if (diag.block != SIZE_MAX)
            fprintf(stderr, ", block %zu", diag.block);
        if (diag.instruction != SIZE_MAX)
            fprintf(stderr, ", instruction %zu", diag.instruction);
        fprintf(stderr, ": %s\n", diag.message);
    } else if (!verify &&
               (fwrite(artifact.data, 1, artifact.size, stdout) != artifact.size || fflush(stdout)))
        fputs("archon-backend: artifact write failed\n", stderr);
    else
        rc = 0;
    archon_backend_artifact_destroy(&artifact);
    free(buffer);
    return rc;
}
