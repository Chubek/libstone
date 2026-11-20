#undef NDEBUG
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "MetaVect.h"

static const char drawing[] =
    "canvas(100,80) { let pos = 1; rect(pos,2,10,20,\"#ff0000\"); "
    "circle(2,3,4,\"#00ff00\"); line(1,2,3,4,\"#0000ff\",1); }";

static void check_empty(const mv_output *output) {
    assert(output->data == NULL && output->size == 0);
}

static void check_status_contract(void) {
    assert(METAVECT_VERSION_MAJOR == 0 && METAVECT_VERSION_MINOR == 1 &&
           METAVECT_VERSION_PATCH == 0);
    assert(strcmp(mv_version_string(), "0.1.0") == 0);
    assert(MV_STATUS_OK == 0 && MV_STATUS_INVALID_ARGUMENT == 1 &&
           MV_STATUS_OUT_OF_MEMORY == 2 && MV_STATUS_NOT_IMPLEMENTED == 3 &&
           MV_STATUS_SYNTAX_ERROR == 4 && MV_STATUS_SEMANTIC_ERROR == 5 &&
           MV_STATUS_BACKEND_ERROR == 6 && MV_STATUS_LIMIT_EXCEEDED == 7);
    assert(MV_BACKEND_SVG == 0 && MV_BACKEND_EPS == 1);
    assert(strcmp(mv_status_string(MV_STATUS_OK), "ok") == 0);
    assert(strcmp(mv_status_string(MV_STATUS_INVALID_ARGUMENT), "invalid argument") == 0);
    assert(strcmp(mv_status_string(MV_STATUS_OUT_OF_MEMORY), "out of memory") == 0);
    assert(strcmp(mv_status_string(MV_STATUS_NOT_IMPLEMENTED), "not implemented") == 0);
    assert(strcmp(mv_status_string(MV_STATUS_SYNTAX_ERROR), "syntax error") == 0);
    assert(strcmp(mv_status_string(MV_STATUS_SEMANTIC_ERROR), "semantic error") == 0);
    assert(strcmp(mv_status_string(MV_STATUS_BACKEND_ERROR), "backend error") == 0);
    assert(strcmp(mv_status_string(MV_STATUS_LIMIT_EXCEEDED), "limit exceeded") == 0);
    assert(strcmp(mv_status_string((mv_status)99), "unknown status") == 0);
}

static void check_output_lifetime(void) {
    char *source = malloc(sizeof drawing - 1);
    mv_output svg, eps;
    mv_diagnostic diag = {99, 99, 99, "stale"};

    assert(source != NULL);
    memcpy(source, drawing, sizeof drawing - 1);
    assert(mv_compile(source, sizeof drawing - 1, MV_BACKEND_SVG, &svg, &diag) == MV_STATUS_OK);
    assert(diag.offset == 0 && diag.line == 0 && diag.column == 0 && diag.message == NULL);
    assert(mv_compile(source, sizeof drawing - 1, MV_BACKEND_EPS, &eps, NULL) == MV_STATUS_OK);
    free(source);

    assert(svg.data != NULL && svg.size == strlen(svg.data) && svg.data[svg.size] == '\0');
    assert(strstr(svg.data, "<svg") != NULL);
    assert(strstr(svg.data, "<rect") != NULL);
    assert(strstr(svg.data, "<circle") != NULL);
    assert(strstr(svg.data, "<line") != NULL);
    assert(eps.data != NULL && eps.size == strlen(eps.data) && eps.data[eps.size] == '\0');
    assert(strncmp(eps.data, "%!PS-Adobe-", sizeof "%!PS-Adobe-" - 1) == 0);

    mv_output_destroy(&svg);
    check_empty(&svg);
    mv_output_destroy(&svg);
    check_empty(&svg);
    assert(strncmp(eps.data, "%!PS-Adobe-", sizeof "%!PS-Adobe-" - 1) == 0);
    mv_output_destroy(&eps);
    check_empty(&eps);
    mv_output_destroy(NULL);
}

static void check_source_errors(void) {
    static const char malformed[] = "canvas(100,80) {\n  @\n}";
    static const char embedded_nul[] = "canvas(100,80) {}\0";
    static const char unresolved[] = "canvas(100,80) {\n  rect(missing,2,10,20,\"#ff0000\");\n}";
    mv_output output;
    mv_diagnostic diag;

    assert(mv_compile(malformed, sizeof malformed - 1, MV_BACKEND_SVG, &output, &diag) ==
           MV_STATUS_SYNTAX_ERROR);
    check_empty(&output);
    assert(diag.offset == sizeof "canvas(100,80) {\n  " - 1);
    assert(diag.line == 2 && diag.column == 3);
    assert(diag.message != NULL && *diag.message != '\0');

    assert(mv_compile(embedded_nul, sizeof embedded_nul - 1, MV_BACKEND_SVG, &output, &diag) ==
           MV_STATUS_SYNTAX_ERROR);
    check_empty(&output);
    assert(diag.offset == sizeof embedded_nul - 2 && diag.line == 1);
    assert(diag.column == diag.offset + 1);

    assert(mv_compile(unresolved, sizeof unresolved - 1, MV_BACKEND_EPS, &output, &diag) ==
           MV_STATUS_SEMANTIC_ERROR);
    check_empty(&output);
    assert(diag.line == 2 && diag.column > 0 && diag.message != NULL);

    assert(mv_compile("", 0, MV_BACKEND_SVG, &output, NULL) == MV_STATUS_SYNTAX_ERROR);
    check_empty(&output);
}

static void check_invalid_arguments(void) {
    char sentinel;
    mv_output output = {&sentinel, 42};
    mv_diagnostic diag = {99, 99, 99, "stale"};

    assert(mv_compile(NULL, 0, MV_BACKEND_SVG, &output, &diag) == MV_STATUS_INVALID_ARGUMENT);
    check_empty(&output);
    assert(diag.offset == 0 && diag.line == 0 && diag.column == 0 &&
           diag.message != NULL && strcmp(diag.message, "stale") != 0);

    output = (mv_output){&sentinel, 42};
    assert(mv_compile(drawing, sizeof drawing - 1, (mv_backend)42, &output, &diag) ==
           MV_STATUS_INVALID_ARGUMENT);
    check_empty(&output);
    assert(diag.offset == 0 && diag.line == 0 && diag.column == 0 && diag.message != NULL);

    assert(mv_compile(drawing, sizeof drawing - 1, MV_BACKEND_SVG, NULL, &diag) ==
           MV_STATUS_INVALID_ARGUMENT);
    assert(diag.offset == 0 && diag.line == 0 && diag.column == 0 && diag.message != NULL);
    assert(mv_compile(drawing, sizeof drawing - 1, MV_BACKEND_SVG, NULL, NULL) ==
           MV_STATUS_INVALID_ARGUMENT);
}

int main(void) {
    check_status_contract();
    check_output_lifetime();
    check_source_errors();
    check_invalid_arguments();
    return 0;
}
