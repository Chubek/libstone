#include <cephyr/Cephyr.h>
#include <cephyr/CephyrSDK.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static uint32_t analyze(const cephyr_sdk_input *input, const cephyr_sdk_output *output,
                        void *user)
{
    unsigned *calls = user;
    ++*calls;
    if (input->stage != CEPHYR_SDK_ANALYZE ||
        input->mir_schema_version != CEPHYR_SDK_MIR_SCHEMA_VERSION)
        return CEPHYR_SDK_REJECT;
    const cephyr_sdk_text code = {"sdk.installed", sizeof("sdk.installed") - 1};
    const cephyr_sdk_text message = {"installed callback", sizeof("installed callback") - 1};
    const cephyr_sdk_text file = {0};
    return output->report(output->context, CEPHYR_SDK_NOTE, code, message, file, 0, 0) ?
        CEPHYR_SDK_OK : CEPHYR_SDK_REJECT;
}

static int test_installed_module(const char *source, size_t source_size)
{
    const char *const args[] = {"--target=auto", "-shared", "--stdlib=cephyr",
                                "-nostdlib", "-o", "-", "-", "--no-cache"};
    const cephyr_context_options options = {
        .struct_size = sizeof(options), .argc = sizeof(args) / sizeof(args[0]), .argv = args
    };
    cephyr_context *context = NULL;
    cephyr_result *artifact = NULL;
    if (cephyr_context_create(&options, &context) != CEPHYR_OK)
        return 4;
    const char second[] = "extern int cephyr_installed_consumer(void);\n"
                          "int cephyr_installed_pair(void) { return cephyr_installed_consumer() + 5; }\n";
    int compiled = cephyr_context_add_translation_unit(context, "installed-one.c", source, source_size);
    if (compiled == CEPHYR_OK)
        compiled = cephyr_context_add_translation_unit(context, "installed-two.c", second, sizeof(second) - 1);
    if (compiled == CEPHYR_OK)
        compiled = cephyr_context_link(context, NULL, &artifact);
    cephyr_context_destroy(context);
    if (compiled != CEPHYR_OK) {
        const char *message = cephyr_result_diagnostics(artifact, NULL);
        const int unsupported = message &&
            strstr(message, "no Cephyr native C lowering target for host");
        if (message)
            fputs(message, stderr);
        cephyr_result_destroy(artifact);
        return unsupported ? 0 : 5;
    }
    cephyr_module *module = NULL;
    cephyr_result *details = NULL;
    const int loaded = cephyr_module_load(artifact, &module, &details);
    cephyr_result_destroy(artifact);
    if (details) {
        fputs(cephyr_result_diagnostics(details, NULL), stderr);
        cephyr_result_destroy(details);
    }
    if (loaded != CEPHYR_OK)
        return 6;
    void *address = NULL;
    const int found = cephyr_module_symbol(module, "cephyr_installed_pair", &address);
    const int ok = found == CEPHYR_OK && address && ((int (*)(void))address)() == 42;
    cephyr_module_destroy(module);
    return ok ? 0 : 7;
}

int main(void)
{
    const char *const args[] = {"--target=x86_64-linux-gnu", "-x", "c", "-", "-S",
                                "-o", "-", "--no-cache"};
    const char source[] = "#include <limits.h>\n"
                          "#ifndef CEPHYR_TEST_INSTALLED_HEADER\n"
                          "#error not using installed Cephyr resources\n"
                          "#endif\n"
                          "_Static_assert(CHAR_BIT == 8, \"expected 8-bit bytes\");\n"
                          "int cephyr_installed_consumer(void) { return 37; }\n";
    unsigned calls = 0;
    const cephyr_sdk_hook hook = {CEPHYR_SDK_ANALYZE, {"consumer", 8}, {0}, analyze, &calls};
    const cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, &hook, 1};
    const cephyr_context_options options = {
        .struct_size = sizeof(options), .argc = sizeof(args) / sizeof(args[0]),
        .argv = args,
    };
    if (cephyr_abi_version() != 1 || !cephyr_version() || !*cephyr_version())
        return 1;
    cephyr_context *context = NULL;
    if (cephyr_context_create(&options, &context) != CEPHYR_OK)
        return 2;
    int exit_code = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const cephyr_request request = {
            .struct_size = sizeof(request), .argc = options.argc, .argv = args,
            .stdin_data = source, .stdin_size = sizeof(source) - 1, .sdk = &sdk,
        };
        cephyr_result *result = NULL;
        const int status = i == 0 ? cephyr_compile(&request, &result) :
            cephyr_context_compile(context, source, sizeof(source) - 1,
                                   i == 1 ? &sdk : NULL, &result);
        size_t size = 0;
        const char *output = cephyr_result_output(result, &size);
        const char *diagnostics = cephyr_result_diagnostics(result, NULL);
        const int ok = status == CEPHYR_OK && size && diagnostics &&
                       strstr(output, "cephyr_installed_consumer:") != NULL &&
                       (strstr(diagnostics, "sdk.installed") != NULL) == (i < 2) &&
                       calls == (i == 0 ? 1u : 2u);
        if (!ok) {
            diagnostics = cephyr_result_diagnostics(result, &size);
            if (size && diagnostics)
                fwrite(diagnostics, 1, size, stderr);
        }
        cephyr_result_destroy(result);
        if (!ok) {
            exit_code = 3;
            break;
        }
    }
    cephyr_context_destroy(context);
    return exit_code ? exit_code : test_installed_module(source, sizeof(source) - 1);
}
