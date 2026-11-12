#include <core/moonstone.h>

int main(void) {
    ms_v1_source *source = NULL;
    ms_v1_span span;
    if (ms_v1_api_version() != 1)
        return 1;
    if (ms_v1_source_create("c", 1, "x\ny", 3, 4, &source) != MS_V1_OK)
        return 2;
    ms_v1_status status = ms_v1_source_span(source, 0, 3, &span);
    ms_v1_source_destroy(source);
    return status == MS_V1_OK && span.end_line == 2 ? 0 : 3;
}
