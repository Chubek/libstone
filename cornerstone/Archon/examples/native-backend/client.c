/* Host program: build two MIR modules using only the public C ABI. */
#include "Backend.h"

#include <stdio.h>

#define LENGTH(xs) (sizeof(xs) / sizeof((xs)[0]))
#define I64 ARCHON_MIR_I64
#define NONE ARCHON_MIR_NONE

/* choose_max(a, b) uses an edge argument to pick the join's block parameter. */
static const ArchonMIRType max_types[] = {I64, I64, I64, I64};
static const ArchonValue take_left[] = {0};
static const ArchonValue take_right[] = {1};
static const ArchonValue max_join[] = {3};
static const ArchonMIRInst compare[] = {
    {.op = ARCHON_MIR_SLT, .dst = 2, .a = 0, .b = 1},
};
static const ArchonMIRBlock max_blocks[] = {
    {
        .insts = compare, .inst_count = LENGTH(compare),
        .term = {
            .kind = ARCHON_MIR_CBR, .value = 2,
            .yes = {1, take_right, LENGTH(take_right)},
            .no = {1, take_left, LENGTH(take_left)},
        },
    },
    {
        .params = max_join, .param_count = LENGTH(max_join),
        .term = {.kind = ARCHON_MIR_RETURN, .value = 3},
    },
};
static const ArchonMIRFunction choice[] = {
    {
        .name = "choose_max", .result = I64,
        .types = max_types, .value_count = LENGTH(max_types), .param_count = 2,
        .blocks = max_blocks, .block_count = LENGTH(max_blocks),
    },
};
static const ArchonMIRModule choice_module = {
    .version = ARCHON_MIR_SCHEMA_VERSION,
    .functions = choice, .function_count = LENGTH(choice),
};

/* scale_max(a, b) calls the first generated module and then a C function. */
static const ArchonMIRType pair[] = {I64, I64};
static const ArchonMIRType scalar[] = {I64};
static const ArchonMIRImport scale_imports[] = {
    {"choose_max", I64, pair, LENGTH(pair)},
    {"native_scale", I64, scalar, LENGTH(scalar)},
};
static const ArchonMIRType scale_types[] = {I64, I64, I64, I64};
static const ArchonValue max_args[] = {0, 1};
static const ArchonValue scale_args[] = {2};
static const ArchonMIRInst scale_insts[] = {
    {
        .op = ARCHON_MIR_CALL, .dst = 2, .a = NONE, .b = NONE,
        .symbol = "choose_max", .args = max_args, .arg_count = LENGTH(max_args),
    },
    {
        .op = ARCHON_MIR_CALL, .dst = 3, .a = NONE, .b = NONE,
        .symbol = "native_scale", .args = scale_args, .arg_count = LENGTH(scale_args),
    },
};
static const ArchonMIRBlock scale_blocks[] = {
    {
        .insts = scale_insts, .inst_count = LENGTH(scale_insts),
        .term = {.kind = ARCHON_MIR_RETURN, .value = 3},
    },
};
static const ArchonMIRFunction scale[] = {
    {
        .name = "scale_max", .result = I64,
        .types = scale_types, .value_count = LENGTH(scale_types), .param_count = 2,
        .blocks = scale_blocks, .block_count = LENGTH(scale_blocks),
    },
};
static const ArchonMIRModule scale_module = {
    .version = ARCHON_MIR_SCHEMA_VERSION,
    .functions = scale, .function_count = LENGTH(scale),
    .imports = scale_imports, .import_count = LENGTH(scale_imports),
};

static int compile_file(const ArchonMIRModule *module, const char *path)
{
    ArchonBackendArtifact assembly = {0};
    ArchonBackendDiagnostic diagnostic = {0};
    FILE *file;
    int ok;

    if (archon_backend_compile(module, NULL, &assembly, &diagnostic)
            != ARCHON_BACKEND_OK) {
        fprintf(stderr, "backend stage %d: %s\n",
            (int)diagnostic.stage, diagnostic.message);
        return 0;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        perror(path);
        archon_backend_artifact_destroy(&assembly);
        return 0;
    }
    ok = fwrite(assembly.data, 1, assembly.size, file) == assembly.size;
    if (fclose(file) != 0)
        ok = 0;
    if (!ok)
        fprintf(stderr, "could not write %s\n", path);
    archon_backend_artifact_destroy(&assembly);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s choice.s scale.s\n", argv[0]);
        return 2;
    }
    return !(compile_file(&choice_module, argv[1]) &&
             compile_file(&scale_module, argv[2]));
}
