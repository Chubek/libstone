#include <cephyr/CephyrPlugin.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PLUGIN_ABI
#define PLUGIN_ABI CEPHYR_SDK_ABI_VERSION
#endif
#ifndef HOOK_COUNT
#define HOOK_COUNT 1
#endif

#ifdef FIRST_PLUGIN
int cephyr_config_test_local_symbol(void) { return 0; }

__attribute__((constructor))
static void record_load(void)
{
    const char *path = getenv("CEPHYR_CONFIG_TEST_MARKER");
    FILE *marker = path ? fopen(path, "w") : NULL;
    if (marker)
        fclose(marker);
}
#endif

#ifndef NO_ENTRY
static uint32_t desugar(const cephyr_sdk_input *input, const cephyr_sdk_output *output, void *user)
{
    const char *expected = user;
    if (input->text.size != strlen(expected) ||
        memcmp(input->text.data, expected, input->text.size) != 0)
        return CEPHYR_SDK_REJECT;
#ifdef FIRST_PLUGIN
    static const char replacement[] = "int first_plugin;\n";
#else
    if (dlsym(RTLD_DEFAULT, "cephyr_config_test_local_symbol"))
        return CEPHYR_SDK_REJECT;
    static const char replacement[] = "int second_plugin;\n#pragma GCC config_native_value\n";
#endif
    return output->set_text(output->context, (cephyr_sdk_text){replacement, sizeof(replacement) - 1})
        ? CEPHYR_SDK_OK : CEPHYR_SDK_REJECT;
}

#ifndef FIRST_PLUGIN
static uint32_t directive(const cephyr_sdk_input *input, const cephyr_sdk_output *output, void *user)
{
    (void)user;
    static const char replacement[] = "enum { native_value = 7 };\n";
    if (input->stage != CEPHYR_SDK_DIRECTIVE || input->line != 2)
        return CEPHYR_SDK_REJECT;
    return output->set_text(output->context, (cephyr_sdk_text){replacement, sizeof(replacement) - 1})
        ? CEPHYR_SDK_OK : CEPHYR_SDK_REJECT;
}
#endif

const cephyr_sdk *cephyr_plugin_v1(void)
{
#ifdef FIRST_PLUGIN
    static const char name[] = "first_plugin";
    static char expected[] = "int original;\n";
#else
    static const char name[] = "second_plugin";
    static char expected[] = "int first_plugin;\n";
#endif
    static const cephyr_sdk_hook hook = {
        CEPHYR_SDK_DESUGAR, {name, sizeof(name) - 1}, {NULL, 0}, desugar, expected
    };
#ifdef FIRST_PLUGIN
    static const cephyr_sdk sdk = {sizeof(sdk), PLUGIN_ABI, &hook, HOOK_COUNT, NULL, 0};
#else
    static const char directive_name[] = "config_native_value";
    static const cephyr_sdk_directive registration = {
        {directive_name, sizeof(directive_name) - 1}, {NULL, 0}, directive, NULL
    };
    static const cephyr_sdk sdk = {sizeof(sdk), PLUGIN_ABI, &hook, HOOK_COUNT, &registration, 1};
#endif
    return &sdk;
}
#endif
