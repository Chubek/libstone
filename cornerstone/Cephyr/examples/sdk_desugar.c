#include <cephyr/CephyrSDK.h>

#include <stdio.h>
#include <string.h>

#define SPAN(literal) { (literal), sizeof(literal) - 1 }

/* This deliberately recognizes only one complete raw translation unit.
 * Production desugarers need their own tokenization. */
static uint32_t desugar(const cephyr_sdk_input *input, const cephyr_sdk_output *out,
                        void *user)
{
    (void)user;
    const cephyr_sdk_text source = SPAN("export_answer 42;\n");
    if (input->text.size != source.size ||
        memcmp(input->text.data, source.data, source.size) != 0)
        return CEPHYR_SDK_OK;

    const cephyr_sdk_text c = SPAN("int answer(void) { return 42; }\n");
    return out->set_text(out->context, c) ? CEPHYR_SDK_OK : CEPHYR_SDK_REJECT;
}

int main(void)
{
    const cephyr_sdk_hook hook = {
        CEPHYR_SDK_DESUGAR, SPAN("export-answer"), {0}, desugar, NULL
    };
    const cephyr_sdk sdk = {sizeof(sdk), CEPHYR_SDK_ABI_VERSION, &hook, 1};
    const char *args[] = {"-x", "c", "-", "--emit-mir", "-o", "-"};
    const char source[] = "export_answer 42;\n";
    const cephyr_request request = {
        sizeof(request), sizeof(args) / sizeof(args[0]), args,
        source, sizeof(source) - 1, NULL, NULL, &sdk
    };
    cephyr_result *result = NULL;
    const int status = cephyr_compile(&request, &result);
    if (result) {
        fputs(cephyr_result_diagnostics(result, NULL), stderr);
        fputs(cephyr_result_output(result, NULL), stdout);
        cephyr_result_destroy(result);
    }
    return status == CEPHYR_OK ? 0 : 1;
}
