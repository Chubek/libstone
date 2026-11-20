#include <cephyr/Cephyr.h>

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    return 1; \
} } while (0)

int main(void)
{
    const char *const preprocess[] = {"-E", "-P", "-x", "c", "-", "-o", "-"};
    const char *const syntax[] = {"-fsyntax-only", "-x", "c", "-"};
    const char *const sarif[] = {"-fsyntax-only", "--diagnostics-format=sarif", "-"};
    const char *const mir[] = {"--emit-mir", "-x", "c", "-", "-o", "-"};
    const char *const broken_args[] = {NULL};
    const char source[] = "#include <stdint.h>\n#define TEST_VALUE 347\n"
                          "int embedding_c(void) { return TEST_VALUE; }\n";
    const char broken[] = "int embedding_broken(void) { return ; + }";
    /* The source is a length-delimited span, not a C string. */
    char bytes[sizeof(source) - 1];
    memcpy(bytes, source, sizeof(bytes));

    size_t size = 999;
    cephyr_result *first = NULL;
    cephyr_result *result = NULL;
    CHECK(cephyr_abi_version() == 1);
    CHECK(cephyr_version() && cephyr_version()[0]);
    CHECK((cephyr_python_version() >> 16) == 3);
    CHECK(cephyr_result_output(NULL, &size) == NULL && size == 0);
    CHECK(cephyr_result_diagnostics(NULL, NULL) == NULL);
    cephyr_result_destroy(NULL);

    struct {
        cephyr_request request;
        size_t future_field;
    } extended = {
        {sizeof(extended), sizeof(preprocess) / sizeof(*preprocess),
         preprocess, bytes, sizeof(bytes), NULL, NULL, NULL},
        42
    };
    CHECK(cephyr_compile(&extended.request, &first) == CEPHYR_OK && first);
    const char *output = cephyr_result_output(first, &size);
    CHECK(output && size == strlen(output));
    CHECK(strstr(output, "embedding_c") && strstr(output, "347"));
    CHECK(!strstr(output, "TEST_VALUE"));
    CHECK(cephyr_result_diagnostics(first, &size) && size == 0);

    cephyr_request request = extended.request;
    request.struct_size = sizeof(request);
    request.argc = sizeof(syntax) / sizeof(*syntax);
    request.argv = syntax;
    result = first; /* cephyr_compile replaces this slot, not the prior result. */
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result);
    CHECK(result != first);
    CHECK(cephyr_result_output(result, &size) && size == 0);
    cephyr_result_destroy(result);

    request.argc = sizeof(sarif) / sizeof(*sarif);
    request.argv = sarif;
    request.stdin_data = broken;
    request.stdin_size = sizeof(broken) - 1;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    const char *diagnostics = cephyr_result_diagnostics(result, &size);
    CHECK(diagnostics && size == strlen(diagnostics));
    CHECK(strstr(diagnostics, "\"version\":\"2.1.0\"") && strstr(diagnostics, "cephyr.parse"));
    CHECK(cephyr_result_output(result, &size) && size == 0);
    cephyr_result_destroy(result);

    request.argc = sizeof(mir) / sizeof(*mir);
    request.argv = mir;
    request.stdin_data = bytes;
    request.stdin_size = sizeof(bytes);
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result);
    CHECK(strncmp(cephyr_result_output(result, NULL), "archon-mir 1\n", 13) == 0);
    CHECK(strstr(output, "347")); /* first result outlives later calls */
    cephyr_result_destroy(first);
    cephyr_result_destroy(result);

    request.resource_root = "/proc/self/cephyr-nonexistent-root";
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "cephyr.target"));
    cephyr_result_destroy(result);
    request.resource_root = NULL;

    request.archon_root = "/proc/self/cephyr-nonexistent-root";
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "cephyr.target"));
    cephyr_result_destroy(result);
    request.archon_root = NULL;

    bytes[3] = '\0';
    CHECK(cephyr_compile(&request, &result) == CEPHYR_COMPILE_ERROR && result);
    CHECK(strstr(cephyr_result_diagnostics(result, NULL), "NUL"));
    cephyr_result_destroy(result);

    CHECK(cephyr_compile(NULL, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    CHECK(cephyr_compile(&request, NULL) == CEPHYR_INVALID_ARGUMENT);
    request.struct_size = sizeof(request) - 1;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    request.struct_size = sizeof(request);
    request.stdin_data = NULL;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_INVALID_ARGUMENT && !result);
    request.stdin_size = 0;
    request.argc = 1;
    request.argv = broken_args;
    CHECK(cephyr_compile(&request, &result) == CEPHYR_INVALID_ARGUMENT && !result);

    request.argv = syntax;
    request.argc = sizeof(syntax) / sizeof(*syntax);
    CHECK(cephyr_compile(&request, &result) == CEPHYR_OK && result);
    CHECK(cephyr_result_output(result, &size) && size == 0);
    cephyr_result_destroy(result);
    return 0;
}
