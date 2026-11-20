#include "NativeInternal.h"
#include "TargetNative.h"

#include <stdlib.h>
#include <string.h>

/* Artifact byte limits, excluding the sink's terminator. */
#define AN_DEFAULT_OUTPUT_BYTES ((size_t)16 * 1024 * 1024)
#define AN_MAX_OUTPUT_BYTES ((size_t)64 * 1024 * 1024)
#define AN_MAX_SYMBOL_LENGTH 255u

int an_error(ANContext *ctx, ArchonBackendStatus status, const char *message)
{
    ArchonBackendDiagnostic *diag = ctx->diag;
    if (diag->status == ARCHON_BACKEND_OK) {
        diag->status = status;
        diag->stage = ctx->stage;
        diag->function = ctx->function;
        diag->block = ctx->block;
        diag->instruction = ctx->instruction;
        strncpy(diag->message, message, sizeof diag->message - 1);
        diag->message[sizeof diag->message - 1] = '\0';
    }
    return 0;
}

int an_continue(ANContext *ctx)
{
    if (ctx->diag->status != ARCHON_BACKEND_OK)
        return 0;
    if (ctx->options && ctx->options->cancelled &&
        ctx->options->cancelled(ctx->options->user))
        return an_error(ctx, ARCHON_BACKEND_CANCELLED, "native compilation cancelled");
    return 1;
}

static int symbol_head(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

int an_symbol(const char *name)
{
    size_t i;
    unsigned char ch;
    if (!name || !symbol_head((unsigned char)name[0]))
        return 0;
    for (i = 1; i <= AN_MAX_SYMBOL_LENGTH; ++i) {
        ch = (unsigned char)name[i];
        if (ch == '\0')
            return 1;
        if (!symbol_head(ch) && (ch < '0' || ch > '9'))
            return 0;
    }
    return 0;
}

static size_t output_limit(const ArchonBackendOptions *options)
{
    size_t limit = options ? options->max_output_bytes : 0;
    if (!limit)
        return AN_DEFAULT_OUTPUT_BYTES;
    return limit < AN_MAX_OUTPUT_BYTES ? limit : AN_MAX_OUTPUT_BYTES;
}

static void output_error(ANContext *ctx, ArchonBackendStatus status)
{
    const char *message = "native artifact emission failed";
    if (status == ARCHON_BACKEND_LIMIT)
        message = "native artifact output exceeds backend limits";
    else if (status == ARCHON_BACKEND_NOMEM)
        message = "cannot allocate native artifact output";
    ctx->stage = ARCHON_BACKEND_EMIT;
    an_error(ctx, status, message);
}

ArchonBackendStatus archon_backend_compile_ex(const ArchonMIRModule *module,
    const ArchonBackendOptions *options, unsigned flags, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic)
{
    /* Always keep a diagnostic, even when the caller only needs the status. */
    ArchonBackendDiagnostic diag = {
        .status = ARCHON_BACKEND_OK,
        .stage = ARCHON_BACKEND_INPUT,
        .function = SIZE_MAX, .block = SIZE_MAX, .instruction = SIZE_MAX
    };
    ANContext ctx = {
        .options = options, .diag = &diag, .stage = ARCHON_BACKEND_INPUT,
        .function = SIZE_MAX, .block = SIZE_MAX, .instruction = SIZE_MAX, .flags = flags
    };
    ANOut out = {0};
    int compiled;

    if (!artifact || artifact->data || artifact->size) {
        an_error(&ctx, ARCHON_BACKEND_INVALID, "an empty artifact is required");
        goto done;
    }
    if (!module) {
        an_error(&ctx, ARCHON_BACKEND_INVALID, "a Machine IR module is required");
        goto done;
    }
    if (flags & ~(unsigned)(ARCHON_BACKEND_SCHEDULE_PURE |
            ARCHON_BACKEND_PHYSICAL_RECORDS | ARCHON_BACKEND_MACHINE_CODE)) {
        an_error(&ctx, ARCHON_BACKEND_UNSUPPORTED, "unknown backend compile flags");
        goto done;
    }
    if ((flags & ARCHON_BACKEND_PHYSICAL_RECORDS) && (flags & ARCHON_BACKEND_MACHINE_CODE)) {
        an_error(&ctx, ARCHON_BACKEND_INVALID, "choose one backend output mode");
        goto done;
    }
    if (!an_continue(&ctx))
        goto done;

    ctx.stage = ARCHON_BACKEND_VERIFY;
    if (!an_verify(&ctx, module))
        an_error(&ctx, ARCHON_BACKEND_INTERNAL, "Machine IR verification failed without a diagnostic");
    if (!an_continue(&ctx))
        goto done;

    ctx.stage = ARCHON_BACKEND_SELECT;
    ctx.function = ctx.block = ctx.instruction = SIZE_MAX;
    out.limit = output_limit(options);
    out.physical = (flags & ARCHON_BACKEND_PHYSICAL_RECORDS) != 0;
    out.machine_code = (flags & ARCHON_BACKEND_MACHINE_CODE) != 0;
    compiled = an_compile_module(&ctx, module, &out);
    if (out.status != ARCHON_BACKEND_OK)
        output_error(&ctx, out.status);
    else if (!compiled)
        an_error(&ctx, ARCHON_BACKEND_INTERNAL, "native compilation failed without a diagnostic");
    else if (out.size > out.limit)
        output_error(&ctx, ARCHON_BACKEND_LIMIT);
    else if (!out.data || out.size >= out.capacity || out.data[out.size] != '\0')
        output_error(&ctx, ARCHON_BACKEND_INTERNAL);
    if (!an_continue(&ctx))
        goto done;

    artifact->data = out.data;
    artifact->size = out.size;
    out.data = NULL;

done:
    free(out.data);
    if (diagnostic)
        *diagnostic = diag;
    return diag.status;
}

ArchonBackendStatus archon_backend_compile(const ArchonMIRModule *module,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic)
{
    return archon_backend_compile_ex(module, options, 0, artifact, diagnostic);
}

ArchonBackendStatus archon_backend_compile_physical(const ArchonMIRModule *module,
    const ArchonBackendOptions *options, ArchonBackendArtifact *artifact,
    ArchonBackendDiagnostic *diagnostic)
{
    return archon_backend_compile_ex(module, options, ARCHON_BACKEND_PHYSICAL_RECORDS,
        artifact, diagnostic);
}

void archon_backend_artifact_destroy(ArchonBackendArtifact *artifact)
{
    if (artifact) {
        free(artifact->data);
        artifact->data = NULL;
        artifact->size = 0;
    }
}

const char *archon_backend_target(void)
{
    return AN_TARGET_NAME;
}
