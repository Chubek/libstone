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
    cephyr_request request = {
        .struct_size = sizeof(request), .argc = sizeof(args) / sizeof(args[0]),
        .argv = args, .stdin_data = source, .stdin_size = sizeof(source) - 1,
    };
    if (cephyr_abi_version() != 1 || !cephyr_version() || !*cephyr_version())
        return 1;
    for (unsigned i = 0; i < 2; ++i) {
        request.sdk = i == 0 ? &sdk : NULL;
        cephyr_result *result = NULL;
        const int status = cephyr_compile(&request, &result);
        if (!result)
            return 2;
        size_t size = 0;
        const char *output = cephyr_result_output(result, &size);
        const char *diagnostics = cephyr_result_diagnostics(result, NULL);
        const int ok = status == CEPHYR_OK && size &&
                       strstr(output, "cephyr_installed_consumer:") != NULL &&
                       (strstr(diagnostics, "sdk.installed") != NULL) == (i == 0) && calls == 1;
        if (!ok) {
            diagnostics = cephyr_result_diagnostics(result, &size);
            if (size && diagnostics)
                fwrite(diagnostics, 1, size, stderr);
        }
        cephyr_result_destroy(result);
        if (!ok)
            return 3;
    }
    return 0;
}
