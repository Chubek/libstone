#define _POSIX_C_SOURCE 200809L

#include <cephyr/CephyrSDK.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                          \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
        exit(1);                                                                 \
    }                                                                            \
} while (0)
#define SPAN(literal) { (literal), sizeof(literal) - 1 }
#define TEXT(literal) ((cephyr_sdk_text)SPAN(literal))
#define NONE ((cephyr_sdk_text){ NULL, 0 })

#define AMD64 "x86_64-linux-gnu"
#define AARCH64 "aarch64-linux-gnu"
#define CUSTOM "x86_64-cephyrsdk-linux-gnu"
#define SOURCE "sdk_answer;\n"
#define C_SOURCE "unsigned long shared = 5;\n" \
                 "unsigned char sidecar = 9;\n" \
                 "static unsigned long answer(void) { return 6UL * 7UL; }\n"
#define MIR_CONSTANTS "const 0 6\nconst 1 7\nzext 2 0 64\nzext 3 1 64\n"
#define MIR_MUL "mul 4 2 3\n"
#define MIR_FOLDED "const 4 42\n"
#define LOCAL "__cephyr_tu_local_answer"

struct context {
    int step;
    int reject;
    int malformed;
};

static const char *resource_root, *archon_root;

static int equals(cephyr_sdk_text text, const char *expected)
{
    return text.size == strlen(expected) &&
           (!text.size || !memcmp(text.data, expected, text.size));
}

static const char *find(cephyr_sdk_text text, const char *needle)
{
    const size_t length = strlen(needle);
    for (size_t i = 0; length <= text.size && i <= text.size - length; ++i)
        if (!memcmp(text.data + i, needle, length))
            return text.data + i;
    return NULL;
}

/* Ensure the caller can overwrite its scratch buffers immediately after a
 * sink returns; the host may not retain any pointer to the submitted bytes. */
static void clobber(char *text, size_t size)
{
    volatile char *bytes = text;
    while (size--)
        *bytes++ = '?';
}

static void check_call(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                       uint32_t stage)
{
    CHECK(in->struct_size >= sizeof(*in));
    CHECK(out->struct_size >= sizeof(*out));
    CHECK(in->abi_version == CEPHYR_SDK_ABI_VERSION);
    CHECK(out->abi_version == CEPHYR_SDK_ABI_VERSION);
    CHECK(in->stage == stage);
    CHECK(equals(in->input_name, "<stdin>"));
    CHECK(equals(in->target_triple, AMD64));
    CHECK(equals(in->target_architecture, "AMD64"));
    CHECK(equals(in->target_abi, "SysVAMD64"));
    CHECK(in->mir_schema_version == (stage == CEPHYR_SDK_DESUGAR ? 0u :
                                     CEPHYR_SDK_MIR_SCHEMA_VERSION));
    CHECK(!in->directive_name.size && !in->line && !in->column);
    if (stage != CEPHYR_SDK_ASSEMBLY)
        CHECK(in->flags == 0);
}

static uint32_t desugar(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                        void *user)
{
    struct context *ctx = user;
    check_call(in, out, CEPHYR_SDK_DESUGAR);
    CHECK(ctx->step++ == 0);
    CHECK(equals(in->text, SOURCE));
    /* The output still needs CPP and the built-in parser/lowering. */
    char source[] = "#define sdk_c C_TOKEN\n"
                    "unsigned long shared = 5;\n"
                    "unsigned char sidecar = 9;\n"
                    "static unsigned long answer(void) { return sdk_c; }\n";
    const cephyr_sdk_text replacement = ctx->malformed ? TEXT("int @;") :
        (cephyr_sdk_text){source, sizeof(source) - 1};
    CHECK(out->set_text(out->context, replacement));
    clobber(source, sizeof(source));
    return CEPHYR_SDK_OK;
}

static uint32_t desugar_again(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                              void *user)
{
    struct context *ctx = user;
    check_call(in, out, CEPHYR_SDK_DESUGAR);
    CHECK(ctx->step++ == 1);
    const char *token = find(in->text, "C_TOKEN");
    CHECK(token);
    const size_t prefix = (size_t)(token - in->text.data);
    char source[256];
    const char value[] = "6UL * 7UL";
    const size_t suffix = prefix + sizeof("C_TOKEN") - 1;
    const size_t size = prefix + sizeof(value) - 1 + in->text.size - suffix;
    CHECK(size <= sizeof(source));
    memcpy(source, in->text.data, prefix);
    memcpy(source + prefix, value, sizeof(value) - 1);
    memcpy(source + prefix + sizeof(value) - 1, in->text.data + suffix,
           in->text.size - suffix);
    CHECK(out->set_text(out->context, (cephyr_sdk_text){source, size}));
    clobber(source, size);
    return CEPHYR_SDK_OK;
}

static uint32_t analyze(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                        void *user)
{
    struct context *ctx = user;
    check_call(in, out, CEPHYR_SDK_ANALYZE);
    CHECK(ctx->step++ == 2);
    CHECK(find(in->text, MIR_CONSTANTS MIR_MUL));
    char message[] = "analysis inspected lowered constants";
    CHECK(out->report(out->context, CEPHYR_SDK_WARNING, TEXT("sdk.analysis"),
                      (cephyr_sdk_text){message, sizeof(message) - 1},
                      TEXT("sdk-origin.c"), 3, 7));
    clobber(message, sizeof(message));
    if (ctx->reject == 2) {
        CHECK(out->report(out->context, CEPHYR_SDK_ERROR, TEXT("sdk.policy"),
                          TEXT("explicit SDK policy error"), NONE, 0, 0));
        return CEPHYR_SDK_OK;
    }
    return ctx->reject ? CEPHYR_SDK_REJECT : CEPHYR_SDK_OK;
}

static uint32_t rewrite(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                        void *user)
{
    struct context *ctx = user;
    check_call(in, out, CEPHYR_SDK_REWRITE);
    CHECK(ctx->step++ == 3);
    const char *constants = find(in->text, MIR_CONSTANTS MIR_MUL);
    CHECK(constants);
    const char *multiply = constants + sizeof(MIR_CONSTANTS) - 1;
    const size_t prefix = (size_t)(multiply - in->text.data);
    const size_t suffix = prefix + sizeof(MIR_MUL) - 1;
    /* Fold this exact known pattern (6 * 7) without changing the SSA IDs or
     * count. In the negative case the syntax parses, but SSA verification
     * must reject the invalid operand. */
    const char *folded = ctx->malformed ? "mul 4 2 90\n" : MIR_FOLDED;
    const size_t length = strlen(folded);
    const size_t size = prefix + length + in->text.size - suffix;
    char mir[1024];
    CHECK(size <= sizeof(mir));
    memcpy(mir, in->text.data, prefix);
    memcpy(mir + prefix, folded, length);
    memcpy(mir + prefix + length, in->text.data + suffix, in->text.size - suffix);
    CHECK(out->set_text(out->context, (cephyr_sdk_text){mir, size}));
    clobber(mir, size);
    return CEPHYR_SDK_OK;
}

static uint32_t assemble(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                         void *user)
{
    struct context *ctx = user;
    check_call(in, out, CEPHYR_SDK_ASSEMBLY);
    CHECK(ctx->step++ == 4);
    CHECK(in->flags == CEPHYR_SDK_POSITION_INDEPENDENT);
    CHECK(find(in->text, "function " LOCAL " "));
    /* Blackstone can remove the operand definitions and renumber the result. */
    CHECK(find(in->text, " 42\nret "));
    CHECK(!find(in->text, MIR_MUL));
    CHECK(find(in->text, "global shared 1 init 5\n"));
    CHECK(find(in->text, "global sidecar 0 "));
    char assembly[] = "\t.text\n\t.globl " LOCAL "\n" LOCAL ":\n"
                      "\tmovq $42, %rax\n\tret\n"
                      "\t.data\n\t.globl shared\nshared:\n\t.quad 5\n";
    CHECK(out->set_text(out->context, (cephyr_sdk_text){assembly, sizeof(assembly) - 1}));
    clobber(assembly, sizeof(assembly));
    return CEPHYR_SDK_OK;
}

static uint32_t forbidden(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                           void *user)
{
    (void)in;
    (void)out;
    (void)user;
    CHECK(!"unexpected SDK hook invocation");
    return CEPHYR_SDK_REJECT;
}

static uint32_t bad_analysis(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                             void *user)
{
    (void)user;
    check_call(in, out, CEPHYR_SDK_ANALYZE);
    CHECK(!out->set_text(out->context, TEXT("analysis cannot rewrite")));
    return CEPHYR_SDK_OK;
}

static uint32_t bad_context(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                            void *user)
{
    check_call(in, out, CEPHYR_SDK_DESUGAR);
    if (user) {
        int wrong_context = 0;
        CHECK(!out->report(&wrong_context, CEPHYR_SDK_NOTE, TEXT("sdk.note"),
                           TEXT("wrong context"), NONE, 0, 0));
    } else {
        CHECK(!out->set_text(NULL, TEXT("int replacement;\n")));
    }
    return CEPHYR_SDK_OK;
}

static uint32_t reentry(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                        void *user)
{
    (void)user;
    check_call(in, out, CEPHYR_SDK_DESUGAR);
    const char *const args[] = {"--version"};
    const cephyr_request request = {sizeof(request), 1, args, NULL, 0, NULL, NULL, NULL};
    cephyr_result *result = NULL;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_INVALID_ARGUMENT);
    CHECK(!result);
    CHECK(out->set_text(out->context, TEXT("int outer_request;\n")));
    return CEPHYR_SDK_OK;
}

static uint32_t too_many_diagnostics(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                                     void *user)
{
    (void)user;
    check_call(in, out, CEPHYR_SDK_ANALYZE);
    for (size_t i = 0; i < CEPHYR_SDK_MAX_DIAGNOSTICS; ++i)
        CHECK(out->report(out->context, CEPHYR_SDK_NOTE, TEXT("sdk.note"),
                          TEXT("bounded diagnostic"), NONE, 0, 0));
    CHECK(!out->report(out->context, CEPHYR_SDK_NOTE, TEXT("sdk.note"),
                       TEXT("over the diagnostic limit"), NONE, 0, 0));
    return CEPHYR_SDK_OK;
}

static uint32_t missing_output(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                                void *user)
{
    (void)user;
    check_call(in, out, CEPHYR_SDK_ASSEMBLY);
    return CEPHYR_SDK_OK;
}

static uint32_t duplicate_output(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                                  void *user)
{
    (void)user;
    check_call(in, out, CEPHYR_SDK_REWRITE);
    CHECK(out->set_text(out->context, in->text));
    CHECK(!out->set_text(out->context, in->text));
    return CEPHYR_SDK_OK;
}

static uint32_t oversized_output(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                                  void *user)
{
    (void)user;
    check_call(in, out, CEPHYR_SDK_REWRITE);
    /* The size limit must be checked before reading the span. */
    CHECK(!out->set_text(out->context, (cephyr_sdk_text){NULL, CEPHYR_SDK_MAX_MIR_BYTES + 1u}));
    return CEPHYR_SDK_OK;
}

static cephyr_result *run(const cephyr_sdk *sdk, const char *source, size_t argc,
                          const char *const *args, int expected_status)
{
    cephyr_request request = {
        sizeof(request), argc, args, source, strlen(source), resource_root, archon_root, sdk
    };
    cephyr_result *result = NULL;
    const int status = cephyr_compile(&request, &result);
    if (status != expected_status || !result) {
        fprintf(stderr, "unexpected compile status %d (expected %d): %s\n", status, expected_status,
                result ? cephyr_result_diagnostics(result, NULL) : "(no result)");
        exit(1);
    }
    return result;
}

static cephyr_result *compile(const cephyr_sdk *sdk, const char *source, const char *target,
                              const char *stage, const char *output, const char *extra,
                              int expected_status)
{
    const char *args[12] = {"--target", target, "--no-cache", "-x", "c", "-", stage};
    size_t argc = 7;
    if (output) {
        args[argc++] = "-o";
        args[argc++] = output;
    }
    if (extra)
        args[argc++] = extra;
    return run(sdk, source, argc, args, expected_status);
}

static void failed(cephyr_result *result, const char *diagnostic)
{
    size_t size;
    cephyr_result_output(result, &size);
    CHECK(size == 0);
    const char *actual = cephyr_result_diagnostics(result, NULL);
    if (!strstr(actual, diagnostic)) {
        fprintf(stderr, "expected diagnostic containing %s, got:\n%s", diagnostic, actual);
        exit(1);
    }
    cephyr_result_destroy(result);
}

static void test_pipeline(void)
{
    struct context ctx = {0};
    cephyr_sdk_hook hooks[] = {
        {CEPHYR_SDK_ASSEMBLY, SPAN("other-backend"), SPAN(AARCH64), forbidden, &ctx},
        {CEPHYR_SDK_REWRITE, SPAN("constant-fold"), SPAN(AMD64), rewrite, &ctx},
        {CEPHYR_SDK_DESUGAR, SPAN("desugar"), {0}, desugar, &ctx},
        {CEPHYR_SDK_ANALYZE, SPAN("analyze"), {0}, analyze, &ctx},
        {CEPHYR_SDK_DESUGAR, SPAN("ignored"), SPAN(AARCH64), forbidden, &ctx},
        {CEPHYR_SDK_DESUGAR, SPAN("desugar-again"), {0}, desugar_again, &ctx},
        {CEPHYR_SDK_ASSEMBLY, SPAN("amd64-backend"), SPAN(AMD64), assemble, &ctx},
    };
    cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, hooks,
                      sizeof(hooks) / sizeof(hooks[0]), NULL, 0};

    cephyr_result *result = compile(&sdk, SOURCE, AMD64, "-S", "-", "-fPIC", CEPHYR_OK);
    CHECK(ctx.step == 5);
    const char *assembly = cephyr_result_output(result, NULL);
    CHECK(strstr(assembly, "\tmovq $42, %rax\n"));
    CHECK(strstr(assembly, "\t.local " LOCAL "\n"));
    CHECK(!strstr(assembly, "\t.globl " LOCAL "\n"));
    CHECK(strstr(assembly, "shared:\n\t.quad 5\n"));
    CHECK(strstr(assembly, "\t.globl sidecar\n"));
    CHECK(strstr(assembly, "sidecar:\n\t.byte 0x09\n"));
    const char *diagnostics = cephyr_result_diagnostics(result, NULL);
    CHECK(strstr(diagnostics, "sdk-origin.c:3:7:"));
    CHECK(strstr(diagnostics, "analysis inspected lowered constants"));
    cephyr_result_destroy(result);

    /* No matching backend: Cephyr's built-in compiler sees the folded MIR. */
    --sdk.hook_count;
    ctx.step = 0;
    result = compile(&sdk, SOURCE, AMD64, "-S", "-", NULL, CEPHYR_OK);
    CHECK(ctx.step == 4);
    assembly = cephyr_result_output(result, NULL);
    CHECK(strstr(assembly, "$0x2a"));
    CHECK(!strstr(assembly, "imulq"));
    cephyr_result_destroy(result);

    ctx.step = 0;
    result = compile(&sdk, SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_OK);
    CHECK(ctx.step == 3); /* Syntax-only runs analysis, not optimization. */
    cephyr_result_destroy(result);

    ctx.step = 0;
    result = compile(&sdk, SOURCE, AMD64, "--emit-mir", "-", NULL, CEPHYR_OK);
    CHECK(ctx.step == 4);
    CHECK(strstr(cephyr_result_output(result, NULL), " 42\nret "));
    CHECK(!strstr(cephyr_result_output(result, NULL), MIR_MUL));
    cephyr_result_destroy(result);

    ctx.step = 0;
    ctx.reject = 1;
    failed(compile(&sdk, SOURCE, AMD64, "-S", "-", NULL, CEPHYR_COMPILE_ERROR),
           "callback rejected the input");
    CHECK(ctx.step == 3);

    ctx.step = 0;
    ctx.reject = 2;
    failed(compile(&sdk, SOURCE, AMD64, "-S", "-", NULL, CEPHYR_COMPILE_ERROR), "sdk.policy");
    CHECK(ctx.step == 3);

    ctx.step = 0;
    ctx.reject = 0;
    failed(compile(&sdk, SOURCE, AMD64, "-S", "-", "-Werror", CEPHYR_COMPILE_ERROR),
           "analysis inspected lowered constants");
    CHECK(ctx.step == 3);
}

static void test_invalid_registration(void)
{
    cephyr_sdk_hook hooks[] = {
        {CEPHYR_SDK_ASSEMBLY, SPAN("backend"), SPAN(AMD64), forbidden, NULL},
        {CEPHYR_SDK_ASSEMBLY, SPAN("duplicate-backend"), SPAN(AMD64), forbidden, NULL},
    };
    cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, hooks, 2, NULL, 0};
    failed(compile(&sdk, C_SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_INVALID_ARGUMENT),
           "more than one assembly hook");
    hooks[1].target_triple = NONE;
    failed(compile(&sdk, C_SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_INVALID_ARGUMENT),
           "requires an exact target triple");
    hooks[1].stage = CEPHYR_SDK_ANALYZE;
    hooks[1].name = hooks[0].name;
    failed(compile(&sdk, C_SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_INVALID_ARGUMENT),
           "hook name must be unique");
    sdk.hook_count = CEPHYR_SDK_MAX_HOOKS + 1u;
    failed(compile(&sdk, C_SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_INVALID_ARGUMENT),
           "invalid SDK hook count");
    sdk.struct_size = offsetof(cephyr_sdk, abi_version);
    failed(compile(&sdk, C_SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_INVALID_ARGUMENT),
           "incompatible SDK descriptor");
    sdk.struct_size = sizeof(sdk);
    sdk.abi_version = CEPHYR_SDK_ABI_VERSION + 1u;
    failed(compile(&sdk, C_SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_INVALID_ARGUMENT),
           "incompatible SDK descriptor");
}

static void test_invalid_callbacks(void)
{
    struct context ctx = {0, 0, 1};
    cephyr_sdk_hook hook = {CEPHYR_SDK_DESUGAR, SPAN("invalid-source"), {0}, desugar, &ctx};
    cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, &hook, 1, NULL, 0};
    cephyr_result *result = compile(&sdk, SOURCE, AMD64, "-S", "-", NULL, CEPHYR_COMPILE_ERROR);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "cephyr."));
    cephyr_result_destroy(result);
    CHECK(ctx.step == 1);
    hook.callback = bad_context;
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "invalid callback sink context");
    hook.user = NULL;
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "invalid callback sink context");
    hook.callback = reentry;
    result = compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_OK);
    CHECK(strstr(cephyr_result_output(result, NULL), "outer_request"));
    cephyr_result_destroy(result);
    hook.stage = CEPHYR_SDK_ANALYZE;
    hook.callback = bad_analysis;
    failed(compile(&sdk, C_SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_COMPILE_ERROR),
           "analysis hooks cannot replace text");
    hook.callback = too_many_diagnostics;
    failed(compile(&sdk, C_SOURCE, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_COMPILE_ERROR),
           "diagnostic limit exceeded");
    hook.stage = CEPHYR_SDK_ASSEMBLY;
    hook.target_triple = TEXT(AMD64);
    hook.callback = missing_output;
    failed(compile(&sdk, C_SOURCE, AMD64, "-S", "-", NULL, CEPHYR_COMPILE_ERROR),
           "did not submit assembly");
    hook.stage = CEPHYR_SDK_REWRITE;
    hook.callback = duplicate_output;
    failed(compile(&sdk, C_SOURCE, AMD64, "-S", "-", NULL, CEPHYR_COMPILE_ERROR),
           "text may be submitted only once");
    hook.callback = oversized_output;
    failed(compile(&sdk, C_SOURCE, AMD64, "--emit-mir", "-", NULL, CEPHYR_COMPILE_ERROR),
           "text exceeds the stage output limit");
    hook.callback = rewrite;
    hook.user = &ctx;
    ctx.step = 3;
    failed(compile(&sdk, C_SOURCE, AMD64, "-S", "-", NULL, CEPHYR_COMPILE_ERROR),
           "cephyr.sdk.mir");
    CHECK(ctx.step == 4);

    /* A failing native rewrite must also leave a named destination unchanged. */
    char path[] = "cephyr-sdk-test.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const char sentinel[] = "previous contents";
    CHECK(write(fd, sentinel, sizeof(sentinel)) == (ssize_t)sizeof(sentinel));
    CHECK(close(fd) == 0);
    ctx.step = 3;
    failed(compile(&sdk, C_SOURCE, AMD64, "-S", path, NULL, CEPHYR_COMPILE_ERROR),
           "cephyr.sdk.mir");
    FILE *file = fopen(path, "rb");
    CHECK(file);
    char contents[sizeof(sentinel)];
    CHECK(fread(contents, 1, sizeof(contents), file) == sizeof(contents));
    CHECK(!memcmp(contents, sentinel, sizeof(sentinel)));
    CHECK(fgetc(file) == EOF);
    CHECK(fclose(file) == 0);
    CHECK(unlink(path) == 0);

    /* A custom backend cannot bypass validation of a rewritten native input. */
    cephyr_sdk_hook hooks[] = {
        hook, {CEPHYR_SDK_ASSEMBLY, SPAN("must-not-run"), SPAN(AMD64), forbidden, NULL},
    };
    sdk.hooks = hooks;
    sdk.hook_count = 2;
    ctx.step = 3;
    failed(compile(&sdk, C_SOURCE, AMD64, "-S", "-", NULL, CEPHYR_COMPILE_ERROR),
           "cephyr.sdk.mir");
    CHECK(ctx.step == 4);

    ctx.step = 3;
    failed(compile(&sdk, C_SOURCE, AMD64, "--emit-mir", "-", NULL, CEPHYR_COMPILE_ERROR),
           "cephyr.sdk.mir");
    CHECK(ctx.step == 4);
}

static uint32_t custom_backend(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                               void *user)
{
    int *calls = user;
    ++*calls;
    CHECK(in->stage == CEPHYR_SDK_ASSEMBLY);
    CHECK(equals(in->target_triple, CUSTOM));
    CHECK(equals(in->target_architecture, "Example64"));
    CHECK(equals(in->target_abi, "LP64"));
    CHECK(equals(in->input_name, "<stdin>"));
    CHECK(in->flags == CEPHYR_SDK_POSITION_INDEPENDENT);
    CHECK(find(in->text, "function answer "));
    CHECK(find(in->text, "const 0 73\n"));
    return out->set_text(out->context,
        TEXT("\t.text\n\t.globl answer\nanswer:\n\tmovl $73, %eax\n\tret\n")) ?
        CEPHYR_SDK_OK : CEPHYR_SDK_REJECT;
}

static void test_custom_object(void)
{
    const char suffix[] = "/tests/fixtures/sdk/config.toml";
    const size_t root_size = strlen(resource_root);
    char *config = malloc(root_size + sizeof(suffix));
    CHECK(config);
    memcpy(config, resource_root, root_size);
    memcpy(config + root_size, suffix, sizeof(suffix));

    int calls = 0;
    const cephyr_sdk_hook hooks[] = {
        {CEPHYR_SDK_ASSEMBLY, SPAN("not-custom"), SPAN(AMD64), forbidden, NULL},
        {CEPHYR_SDK_ASSEMBLY, SPAN("custom-object"), SPAN(CUSTOM), custom_backend, &calls},
    };
    const cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, hooks, 2, NULL, 0};
    char path[] = "cephyr-sdk-object.XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
    const char *args[] = {"--config", config, "-x", "c", "-", "-fPIC", "-c", "-o", path};
    const char source[] = "#if defined(__x86_64__) || !defined(__LP64__) || !defined(__CHAR_UNSIGNED__)\n"
                          "#error wrong SDK-only C target macros\n"
                          "#endif\n"
                          "int answer(void) { return 73; }\n";
    failed(run(NULL, "int answer(void) { return 73; }\n",
               sizeof(args) / sizeof(args[0]), args, CEPHYR_COMPILE_ERROR),
           "unsupported target for C lowering");
    cephyr_result *result = run(&sdk, source,
                               sizeof(args) / sizeof(args[0]), args, CEPHYR_OK);
    cephyr_result_destroy(result);
    CHECK(calls == 1);
    free(config);

    FILE *object = fopen(path, "rb");
    CHECK(object);
    /* ELF64, little-endian, ET_REL, EM_X86_64. */
    unsigned char header[20];
    CHECK(fread(header, 1, sizeof(header), object) == sizeof(header));
    CHECK(!memcmp(header, "\x7f" "ELF\x02\x01", 6));
    CHECK(header[16] == 1 && header[17] == 0 && header[18] == 62 && header[19] == 0);
    CHECK(fclose(object) == 0);
    CHECK(unlink(path) == 0);
}

static void check_directive(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                            const char *name)
{
    CHECK(in->struct_size >= sizeof(*in));
    CHECK(out->struct_size >= sizeof(*out));
    CHECK(in->abi_version == CEPHYR_SDK_ABI_VERSION);
    CHECK(out->abi_version == CEPHYR_SDK_ABI_VERSION);
    CHECK(in->stage == CEPHYR_SDK_DIRECTIVE);
    CHECK(equals(in->target_triple, AMD64));
    CHECK(equals(in->target_architecture, "AMD64"));
    CHECK(equals(in->target_abi, "SysVAMD64"));
    CHECK(equals(in->directive_name, name));
    CHECK(in->mir_schema_version == 0 && in->flags == 0);
    CHECK(in->column == 0 && in->line != 0);
}

static uint32_t emit_directive(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                               void *user)
{
    int *calls = user;
    check_directive(in, out, "emit");
    switch ((*calls)++) {
    case 0: {
        CHECK(equals(in->input_name, "sdk-directive-root.c"));
        CHECK(in->line == 40);
        CHECK(equals(in->text, "VALUE (2, \"// not trivia\")"));
        char text[] = "int GENERATED = 4;\nint also_generated = 5;";
        CHECK(out->set_text(out->context, (cephyr_sdk_text){text, sizeof(text) - 1}));
        clobber(text, sizeof(text));
        break;
    }
    case 1:
        CHECK(equals(in->input_name, "sdk-directive-header.h"));
        CHECK(in->line == 18);
        CHECK(equals(in->text, "header"));
        CHECK(out->set_text(out->context, TEXT("int header_generated = 6;")));
        break;
    case 2:
        CHECK(equals(in->input_name, "sdk-directive-root.c"));
        CHECK(in->line == 71);
        CHECK(equals(in->text, "prefix(7, 8) \"quoted value\""));
        CHECK(out->set_text(out->context, TEXT("20 +")));
        CHECK(out->report(out->context, CEPHYR_SDK_WARNING, TEXT("test.directive"),
                          TEXT("defaulted to the pragma"), NONE, 0, 0));
        break;
    case 3:
        CHECK(equals(in->input_name, "sdk-directive-root.c"));
        CHECK(in->line == 72);
        CHECK(equals(in->text, ""));
        CHECK(out->set_text(out->context, NONE));
        break;
    default:
        CHECK(!"unexpected directive invocation");
    }
    return CEPHYR_SDK_OK;
}

static uint32_t directive_output(const cephyr_sdk_input *in, const cephyr_sdk_output *out,
                                void *user)
{
    check_directive(in, out, "output");
    const cephyr_sdk_text *text = user;
    if (equals(in->text, "reject"))
        return CEPHYR_SDK_REJECT;
    if (equals(in->text, "error")) {
        CHECK(out->report(out->context, CEPHYR_SDK_ERROR, TEXT("test.directive_error"),
                          TEXT("explicit directive error"), NONE, 0, 0));
    } else if (equals(in->text, "invalid")) {
        CHECK(!out->set_text(out->context, *text));
    } else if (equals(in->text, "twice")) {
        CHECK(out->set_text(out->context, NONE));
        CHECK(!out->set_text(out->context, NONE));
    } else {
        CHECK(out->set_text(out->context, *text));
    }
    return CEPHYR_SDK_OK;
}

static void test_directives(void)
{
    int calls = 0;
    const cephyr_sdk_directive directives[] = {
        {SPAN("emit"), {0}, forbidden, NULL},
        {SPAN("emit"), SPAN(AMD64), emit_directive, &calls},
        {SPAN("emit"), SPAN(AARCH64), forbidden, NULL},
        {SPAN("other_target"), SPAN(AARCH64), forbidden, NULL},
    };
    const cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, NULL, 0, directives, 4};
    char include[4096];
    const int length = snprintf(include, sizeof(include), "-I%s/tests/fixtures/sdk", resource_root);
    CHECK(length > 0 && (size_t)length < sizeof(include));
    const char source[] =
        "#define VALUE 999\n"
        "#define GENERATED wrong_name\n"
        "#define SDK_PRAGMA(x) _Pragma(#x)\n"
        "#pragma unrelated cephyr unknown\n"
        "#pragma cephyr_like unknown\n"
        "#pragma Cephyr unknown\n"
        "#pragma GCC diagnostic push\n"
        "_Pragma(\"GCC diagnostic pop\")\n"
        "#pragma GCC other_target\n"
        "#pragma GCC\n"
        "#pragma gcc emit\n"
        "/* #pragma cephyr unknown */\n"
        "const char *lookalike = \"#pragma cephyr unknown _Pragma(\\\"cephyr unknown\\\")\";\n"
        "#if 0\n#pragma cephyr unknown\n_Pragma(\"cephyr unknown\")\n#pragma GCC emit\n#endif\n"
        "#line 40 \"sdk-directive-root.c\"\n"
        "%:pragma /* gap */ GCC emit VALUE /* gap */ (2, \\\n"
        "  \"// not trivia\")\n"
        "#include \"directive.h\"\n"
        "#include \"directive.h\"\n"
        "#line 71 \"sdk-directive-root.c\"\n"
        "int inline_result = 10 + SDK_PRAGMA(GCC emit prefix(7, 8) \"quoted value\") 3;\n"
        "_Pragma(\"cephyr emit\")\n";
    cephyr_result *result = compile(&sdk, source, AMD64, "-E", "-", include, CEPHYR_OK);
    CHECK(calls == 4);
    const char *text = cephyr_result_output(result, NULL);
    const char *first = strstr(text, "int GENERATED = 4;");
    const char *header = strstr(text, "int header_generated = 6;");
    const char *last = strstr(text, "int inline_result = 10 +");
    CHECK(first && header && last && first < header && header < last);
    CHECK(strstr(text, "int also_generated = 5;"));
    CHECK(strstr(text, "\"#pragma cephyr unknown _Pragma"));
    CHECK(!strstr(text, "wrong_name"));
    CHECK(strstr(last, "20 +"));
    CHECK(strstr(text, "# 18 \"sdk-directive-header.h\""));
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "sdk-directive-root.c:71: warning"));
    cephyr_result_destroy(result);

    const char *plain_args[] = {"--target", AMD64, "-E", "-P", "-o", "-", "-x", "c", "-", include};
    calls = 0;
    result = run(&sdk, source, sizeof(plain_args) / sizeof(plain_args[0]), plain_args, CEPHYR_OK);
    text = cephyr_result_output(result, NULL);
    CHECK(calls == 4 && !strstr(text, "\"sdk-directive-header.h\""));
    CHECK(strstr(text, "int header_generated = 6;"));
    cephyr_result_destroy(result);

    calls = 0;
    result = compile(&sdk, source, AMD64, "-fsyntax-only", NULL, include, CEPHYR_OK);
    CHECK(calls == 4);
    cephyr_result_destroy(result);

    /* Warning promotion stops the stream before the following pragma. */
    const char *args[] = {"--target", AMD64, "-E", "-o", "-", "-x", "c", "-", include, "-Werror"};
    calls = 0;
    failed(run(&sdk, source, sizeof(args) / sizeof(args[0]), args, CEPHYR_COMPILE_ERROR),
           "sdk-directive-root.c:71: error");
    CHECK(calls == 3);

    failed(compile(NULL, "#pragma cephyr unknown\n", AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "unknown Cephyr directive 'unknown'");
    result = compile(NULL, "#pragma GCC emit\n_Pragma(\"GCC emit\")\nint after;\n",
                     AMD64, "-E", "-", NULL, CEPHYR_OK);
    CHECK(strstr(cephyr_result_output(result, NULL), "int after;"));
    CHECK(!*cephyr_result_diagnostics(result, NULL));
    cephyr_result_destroy(result);
    failed(compile(&sdk, "#pragma cephyr\n", AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "expected a directive name");
    failed(compile(&sdk, "#pragma cephyr 123\n", AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "expected a directive name");
    failed(compile(&sdk, "#pragma cephyr other_target\n", AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "unknown Cephyr directive 'other_target'");
    failed(compile(&sdk, "#if _Pragma(\"cephyr emit\") 1\n#endif\n", AMD64, "-E", "-", NULL,
                   CEPHYR_COMPILE_ERROR), "not supported inside a preprocessing directive");
    result = compile(NULL, "#define SHARP %:\nSHARP pragma cephyr no_match\n",
                     AMD64, "-E", "-", NULL, CEPHYR_OK);
    CHECK(strstr(cephyr_result_output(result, NULL), "pragma cephyr no_match"));
    cephyr_result_destroy(result);

    const char *cpp_args[] = {"--target", AMD64, "-E", "-o", "-", "-x", "cpp-output", "-"};
    result = run(&sdk, "#pragma cephyr emit\n", sizeof(cpp_args) / sizeof(cpp_args[0]),
                 cpp_args, CEPHYR_OK);
    CHECK(strstr(cephyr_result_output(result, NULL), "#pragma cephyr emit"));
    cephyr_result_destroy(result);
    const char *asm_args[] = {"--target", AMD64, "-E", "-o", "-", "-x", "assembler-with-cpp", "-"};
    result = run(&sdk, "#pragma cephyr emit\n", sizeof(asm_args) / sizeof(asm_args[0]),
                 asm_args, CEPHYR_OK);
    cephyr_result_destroy(result);
    CHECK(calls == 3);
}

static void test_directive_errors(void)
{
    cephyr_sdk_text text = TEXT("int generated;\nint second;");
    cephyr_sdk_directive directive = {SPAN("output"), {0}, directive_output, &text};
    cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, NULL, 0, &directive, 1};
    const char source[] = "#line 36 \"directive-locations.c\"\n"
                          "#pragma cephyr output\n"
                          "int x(void) { return not_declared; }\n";
    failed(compile(&sdk, source, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_COMPILE_ERROR),
           "directive-locations.c:37:");
    failed(compile(&sdk,
                   "#line 79 \"inline-locations.c\"\nint x(void) { _Pragma(\"cephyr output\") "
                   "return not_declared; }\n", AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_COMPILE_ERROR),
           "inline-locations.c:79:");
    text = TEXT("int first;\nint broken = ;");
    failed(compile(&sdk, source, AMD64, "-fsyntax-only", NULL, NULL, CEPHYR_COMPILE_ERROR),
           "directive-locations.c:37:");
    failed(compile(&sdk, "#pragma cephyr output reject\n", AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "<stdin>:1: error[cephyr.sdk.callback]");
    failed(compile(&sdk, "#pragma cephyr output error\n", AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "test.directive_error");
    failed(compile(&sdk, "#pragma cephyr output twice\n", AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "text may be submitted only once");
    const cephyr_sdk_text invalid[] = {
        TEXT("#include \"must-not-open.h\"\n"),
        TEXT("%: 8 \"forged.c\"\n"),
        TEXT("_Pragma(\"cephyr output\")"),
        TEXT("/* eats original source"),
        TEXT("int x; /* no phase-3 trivia */"),
        TEXT("\"unterminated"),
        TEXT("int x;\\\n"),
        TEXT("i\0nt x;"),
        {NULL, CEPHYR_SDK_MAX_DIRECTIVE_OUTPUT_BYTES + 1},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        text = invalid[i];
        failed(compile(&sdk, "#pragma cephyr output invalid\n", AMD64, "-E", "-", NULL,
                       CEPHYR_COMPILE_ERROR), "<stdin>:1: error[cephyr.sdk.callback]");
    }
    const char prefix[] = "#pragma cephyr output ";
    const size_t argument_size = CEPHYR_SDK_MAX_DIRECTIVE_ARGUMENT_BYTES + 1u;
    char *oversized = malloc(sizeof(prefix) + argument_size + 1);
    CHECK(oversized);
    memcpy(oversized, prefix, sizeof(prefix) - 1);
    memset(oversized + sizeof(prefix) - 1, 'a', argument_size);
    memcpy(oversized + sizeof(prefix) - 1 + argument_size, "\n", 2);
    failed(compile(&sdk, oversized, AMD64, "-E", "-", NULL, CEPHYR_COMPILE_ERROR),
           "directive arguments exceed the size limit");
    free(oversized);

    /* A descriptor allocated by a v1 client may end at hook_count. */
    struct original_sdk {
        size_t struct_size;
        uint32_t abi_version;
        const cephyr_sdk_hook *hooks;
        size_t hook_count;
    } original = {sizeof(original), CEPHYR_SDK_ABI_VERSION, NULL, 0};
    CHECK(sizeof(original) == offsetof(cephyr_sdk, directives));
    cephyr_result *result = compile((const cephyr_sdk *)(const void *)&original, C_SOURCE,
                                    AMD64, "-E", "-", NULL, CEPHYR_OK);
    cephyr_result_destroy(result);
    sdk.struct_size = offsetof(cephyr_sdk, directive_count);
    result = compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_OK);
    cephyr_result_destroy(result);
    sdk.struct_size = sizeof(sdk);

    cephyr_sdk_directive entries[] = {directive, directive};
    sdk.directives = entries;
    sdk.directive_count = 2;
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_INVALID_ARGUMENT),
           "directive name and target pair must be unique");
    sdk.directive_count = 1;
    entries[0].name = TEXT("bad-name");
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_INVALID_ARGUMENT),
           "invalid directive name");
    entries[0] = directive;
    entries[0].target_triple = TEXT("not_a_triple");
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_INVALID_ARGUMENT),
           "invalid target triple");
    entries[0] = directive;
    entries[0].callback = NULL;
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_INVALID_ARGUMENT),
           "missing callback");
    sdk.directives = NULL;
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_INVALID_ARGUMENT),
           "missing directive array");
    sdk.directives = entries;
    sdk.directive_count = CEPHYR_SDK_MAX_DIRECTIVES + 1u;
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_INVALID_ARGUMENT),
           "invalid SDK directive count");
    const cephyr_sdk_hook hook = {CEPHYR_SDK_DIRECTIVE, SPAN("not-a-hook"), {0}, forbidden, NULL};
    sdk.directive_count = 0;
    sdk.hooks = &hook;
    sdk.hook_count = 1;
    failed(compile(&sdk, C_SOURCE, AMD64, "-E", "-", NULL, CEPHYR_INVALID_ARGUMENT),
           "unknown stage");
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    resource_root = argv[1];
    archon_root = argv[2];
    test_pipeline();
    test_invalid_registration();
    test_invalid_callbacks();
    test_custom_object();
    test_directives();
    test_directive_errors();
    /* Request-local hooks must not affect later requests. */
    cephyr_result *result = compile(NULL, "unsigned long fresh(void) { return 6UL * 8UL; }\n",
                                    AMD64, "--emit-mir", "-", NULL, CEPHYR_OK);
    CHECK(strstr(cephyr_result_output(result, NULL), "function fresh "));
    CHECK(strstr(cephyr_result_output(result, NULL), " 48\nret "));
    cephyr_result_destroy(result);
    return 0;
}
