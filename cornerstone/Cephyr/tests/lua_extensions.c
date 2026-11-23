#include <cephyr/Cephyr.h>
#include <cephyr/CephyrSDK.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    return EXIT_FAILURE; \
} } while (0)

static uint32_t native_hook(const cephyr_sdk_input *input, const cephyr_sdk_output *output,
                            void *user)
{
    (void)output;
    if (input->stage != CEPHYR_SDK_DESUGAR)
        return CEPHYR_SDK_REJECT;
    ++*(int *)user;
    return CEPHYR_SDK_OK;
}

int main(void)
{
    static const char *const argv[] = {"-E", "-P", "-x", "c", "-"};
    static const char source[] = "int answer = 42;\n";
    static const char extension[] =
        "local sdk = require('lcephyr')\n"
        "assert(sdk.env.get('CEPHYR_TEST_VALUE') == 'granted')\n"
        "assert(sdk.env.get('PATH') == nil)\n";
    static const cephyr_lua_script script = {extension, sizeof(extension) - 1, "request.lua"};
    static const cephyr_lua_env_entry env = {"CEPHYR_TEST_VALUE", "granted"};
    cephyr_lua_options lua = {sizeof(lua), &script, 1, &env, 1};
    int native_calls = 0;
    const cephyr_sdk_hook hook = {
        CEPHYR_SDK_DESUGAR, {"native_hook", 11}, {NULL, 0}, native_hook, &native_calls
    };
    const cephyr_sdk native = {
        .struct_size = offsetof(cephyr_sdk, directives), /* Old SDK ABI prefix. */
        .abi_version = CEPHYR_SDK_ABI_VERSION,
        .hooks = &hook,
        .hook_count = 1,
    };
    cephyr_request request = {
        .struct_size = sizeof(request),
        .argc = sizeof(argv) / sizeof(*argv),
        .argv = argv,
        .stdin_data = source,
        .stdin_size = sizeof(source) - 1,
        .sdk = &native,
        .lua = &lua,
    };
    cephyr_result *result = NULL;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result);
    CHECK(strstr(cephyr_result_output(result, NULL), "int answer = 42;") && native_calls == 1);
    cephyr_result_destroy(result);

    lua.script_count = CEPHYR_LUA_MAX_SCRIPTS + 1;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    request.struct_size = offsetof(cephyr_request, lua); /* Old request ABI prefix. */
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result && native_calls == 2);
    cephyr_result_destroy(result);

    request.struct_size = sizeof(request);
    lua.script_count = 1;
    const cephyr_lua_env_entry duplicated[] = {env, env};
    lua.environment = duplicated;
    lua.environment_count = 2;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "duplicate Lua environment entry"));
    cephyr_result_destroy(result);

    lua.environment = NULL;
    lua.environment_count = 0;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "request.lua") && native_calls == 2);
    cephyr_result_destroy(result);
    return EXIT_SUCCESS;
}
