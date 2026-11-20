#include "mir_verify.h"

#include <NativeInternal.h>
#include <string.h>

/* The shared verifier uses only stack alignment from the ABI record. All
 * supported Cephyr native LP64 providers have 16-byte stack alignment. */
const ANABI an_abi = {.stack_align = 16};

int an_error(ANContext *ctx, ArchonBackendStatus status, const char *message)
{
    ArchonBackendDiagnostic *diagnostic = ctx->diag;
    if (diagnostic->status == ARCHON_BACKEND_OK) {
        diagnostic->status = status;
        diagnostic->stage = ctx->stage;
        diagnostic->function = ctx->function;
        diagnostic->block = ctx->block;
        diagnostic->instruction = ctx->instruction;
        strncpy(diagnostic->message, message, sizeof(diagnostic->message) - 1);
        diagnostic->message[sizeof(diagnostic->message) - 1] = '\0';
    }
    return 0;
}

int an_continue(ANContext *ctx)
{
    return ctx->diag->status == ARCHON_BACKEND_OK;
}

static int symbol_head(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

int an_symbol(const char *name)
{
    size_t i;
    if (!name || !symbol_head((unsigned char)name[0]))
        return 0;
    for (i = 1; i <= 255; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == '\0')
            return 1;
        if (!symbol_head(ch) && (ch < '0' || ch > '9'))
            return 0;
    }
    return 0;
}

/* The shared MIR reader calls this private sink after parsing. The Archon
 * generated backend has a separate implementation in its own DSO. */
ArchonBackendStatus archon_backend_compile_ex(const ArchonMIRModule *module,
    const ArchonBackendOptions *options, unsigned flags,
    ArchonBackendArtifact *artifact, ArchonBackendDiagnostic *diagnostic)
{
    ANContext ctx = {.options = options, .diag = diagnostic, .stage = ARCHON_BACKEND_VERIFY,
        .function = SIZE_MAX, .block = SIZE_MAX, .instruction = SIZE_MAX, .flags = flags};
    (void)artifact;
    if (!an_verify(&ctx, module) && !diagnostic->status)
        an_error(&ctx, ARCHON_BACKEND_INTERNAL, "MachineIR verifier failed without a diagnostic");
    return diagnostic->status;
}

ArchonBackendStatus cephyr_verify_mir(const char *text, size_t size,
                                      ArchonBackendDiagnostic *diagnostic)
{
    ArchonBackendArtifact unused = {0};
    return archon_backend_compile_text(text, size, NULL, &unused, diagnostic);
}
