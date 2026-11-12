#include <assert.h>
#include <core/moonstone.h>
#include <stdint.h>

int main(void) {
    ms_v1_source *source = NULL;
    ms_v1_span span;
    const char text[] = "a\xc3\xa9\nx";
    assert(ms_v1_api_version() == 1);
    assert(ms_v1_source_create("c.ex", 4, text, sizeof(text) - 1, 32, &source) == MS_V1_OK);
    assert(source != NULL);
    assert(ms_v1_source_span(source, 1, 5, &span) == MS_V1_OK);
    assert(span.line == 1 && span.column == 2 && span.end_line == 2 && span.end_column == 2);
    assert(ms_v1_source_span(source, 2, 3, &span) == MS_V1_INVALID_INPUT);
    assert(span.line == 0 && span.end_column == 0);
    assert(ms_v1_source_span(source, 0, SIZE_MAX, &span) == MS_V1_INVALID_INPUT);
    assert(ms_v1_source_span(source, 0, 0, NULL) == MS_V1_INVALID_INPUT);
    ms_v1_source_destroy(source);
    source = NULL;
    assert(ms_v1_source_create(NULL, 0, NULL, 0, 0, &source) == MS_V1_OK);
    assert(ms_v1_source_span(source, 0, 0, &span) == MS_V1_OK && span.column == 1);
    ms_v1_source_destroy(source);
    assert(ms_v1_source_create(NULL, 1, NULL, 0, 4, &source) == MS_V1_INVALID_INPUT);
    assert(source == NULL);
    assert(ms_v1_source_create("", 0, "\xc0\x80", 2, 2, &source) == MS_V1_INVALID_INPUT);
    assert(source == NULL);
    assert(ms_v1_source_create("x", 1, "", 0, 0, &source) == MS_V1_BUDGET_EXHAUSTED);
    assert(ms_v1_source_create("x", SIZE_MAX, "x", 1, SIZE_MAX, &source) == MS_V1_BUDGET_EXHAUSTED);
    assert(ms_v1_source_create(NULL, 0, NULL, 0, 0, NULL) == MS_V1_INVALID_INPUT);
    assert(ms_v1_source_span(NULL, 0, 0, &span) == MS_V1_INVALID_INPUT);
    ms_v1_source_destroy(NULL);
    return 0;
}
