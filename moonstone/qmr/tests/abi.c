/* C ABI conformance test: compiled as C so the header is validated against
 * a real C compiler, not just C++ extern "C" semantics. */
#include "qmr/qmr_abi.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "qmr abi: FAILED: %s\n", what);
        ++failures;
    }
}

int main(void) {
    check(qmr_abi_version() == QMR_ABI_VERSION, "ABI version matches header");

    qmr_engine *engine = qmr_engine_create(NULL, QMR_CODEC_HTML);
    check(engine != NULL, "engine created with defaults");
    if (!engine) return 1;

    {
        const char *keys[] = {"name"};
        const char *values[] = {"Q & R"};
        char *out = NULL;
        char *error = NULL;
        const int rc = qmr_engine_render(engine, "Hello {{ data.name }}!",
                                         "abi.qmr", keys, values, 1, &out, &error);
        check(rc == 0, "render succeeded");
        check(out != NULL && strcmp(out, "Hello Q &amp; R!") == 0,
              "HTML escaping through the C ABI");
        qmr_free(out);
    }

    {
        /* NULL value must model a nil, not a crash. */
        const char *keys[] = {"name"};
        const char *values[] = {NULL};
        char *out = NULL;
        char *error = NULL;
        const int rc = qmr_engine_render(engine, "[{{ data.name }}]", NULL,
                                         keys, values, 1, &out, &error);
        check(rc == 0, "nil value render succeeded");
        check(out != NULL && strcmp(out, "[]") == 0, "nil renders as empty");
        qmr_free(out);
    }

    {
        /* A broken template reports an error buffer instead of crashing. */
        char *out = NULL;
        char *error = NULL;
        const int rc = qmr_engine_render(engine, "{{ no", NULL, NULL, NULL, 0,
                                         &out, &error);
        check(rc != 0, "broken template rejected");
        check(error != NULL && strstr(error, "QMR_UNCLOSED") != NULL,
              "error carries the QMR diagnostic");
        check(out == NULL, "no output on failure");
        qmr_free(error);
        qmr_free(out); /* must tolerate NULL */
    }

    {
        qmr_engine *json_engine = qmr_engine_create(NULL, QMR_CODEC_JSON);
        check(json_engine != NULL, "JSON codec engine");
        if (json_engine) {
            const char *keys[] = {"s"};
            const char *values[] = {"a\"b"};
            char *out = NULL;
            char *error = NULL;
            const int rc = qmr_engine_render(json_engine, "[{{ data.s }}]", NULL,
                                             keys, values, 1, &out, &error);
            check(rc == 0, "JSON codec render succeeded");
            check(out != NULL && strcmp(out, "[\"a\\\"b\"]") == 0,
                  "JSON codec escaping through the C ABI");
            qmr_free(out);
            qmr_engine_destroy(json_engine);
        }
    }

    {
        /* Zeroed limits struct keeps defaults; bad codec is rejected. */
        qmr_limits zeroed;
        memset(&zeroed, 0, sizeof zeroed);
        zeroed.max_output_bytes = 4;
        qmr_engine *small = qmr_engine_create(&zeroed, QMR_CODEC_HTML);
        check(small != NULL, "engine with zeroed limits");
        if (small) {
            const char *keys[] = {"name"};
            const char *values[] = {"x"};
            char *out = NULL;
            char *error = NULL;
            const int rc = qmr_engine_render(small, "0123456789", NULL,
                                             keys, values, 1, &out, &error);
            check(rc != 0, "output limit enforced through ABI");
            check(error != NULL && strstr(error, "QMR_OUTPUT_LIMIT") != NULL,
                  "limit diagnostic surfaced");
            qmr_free(error);
            qmr_engine_destroy(small);
        }
        check(qmr_engine_create(NULL, 99) == NULL, "invalid codec rejected");
    }

    check(strcmp(qmr_version_string(), "") != 0, "version string nonempty");
    qmr_engine_destroy(engine);
    qmr_engine_destroy(NULL); /* must tolerate NULL */

    if (failures == 0) fprintf(stderr, "qmr abi: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
