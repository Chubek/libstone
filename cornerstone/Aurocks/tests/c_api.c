#include "aurocks/API.h"

#include <string.h>

int main(void) {
    const char valid[] = "%start input\n%%\ninput: 'x' { $$ = (void *)1; };\n";
    const char invalid[] = "%start input\n%%\ninput: missing;\n";
    size_t length = 0;
    aurocks_options options = {0};
    aurocks_analysis analysis;
    aurocks_diagnostic diagnostic;
    const char *source;
    aurocks_result *result;
    options.entrypoint = "c_api_parse";
    result = aurocks_compile(valid, strlen(valid), &options);
    source = aurocks_result_source(result, &length);
    if (!aurocks_result_ok(result) || !source || !length ||
        strstr(source, "c_api_parse") == NULL ||
        !aurocks_result_analysis(result, &analysis) ||
        strcmp(analysis.backend, "portable") != 0) {
        aurocks_result_destroy(result);
        return 1;
    }
    aurocks_result_destroy(result);
    result = aurocks_compile(invalid, strlen(invalid), NULL);
    if (!result || aurocks_result_ok(result) ||
        !aurocks_result_diagnostic(result, 0, &diagnostic) ||
        diagnostic.severity != AUROCKS_ERROR || !diagnostic.message) {
        aurocks_result_destroy(result);
        return 2;
    }
    aurocks_result_destroy(result);
    options.check_only = 1;
    result = aurocks_compile(valid, strlen(valid), &options);
    source = aurocks_result_source(result, &length);
    if (!aurocks_result_ok(result) || !source || length || *source) {
        aurocks_result_destroy(result);
        return 3;
    }
    aurocks_result_destroy(result);
    return 0;
}
