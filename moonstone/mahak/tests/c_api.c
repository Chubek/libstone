/* Strict C11 consumer of the Mahak embedding ABI. */
#include <stdio.h>
#include <string.h>

#include "mahak/mahak.h"

static int failures = 0;

static void check(int condition, const char *message) {
    if (condition)
        return;
    fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

int main(void) {
    check(mahak_version() != NULL, "version string is not null");
    check(strlen(mahak_version()) > 0, "version string is not empty");
    check(mahak_api_version() == MAHAK_C_API_VERSION, "C API version probe matches the header");
    check(mahak_result_schema() != NULL, "result schema is available");
    check(strstr(mahak_result_schema(), "schema") != NULL, "result schema mentions schema");

    char *error = NULL;
    check(mahak_validate_manifest("/definitely/not/a/manifest.json", &error) == 2,
          "missing manifest validates as a configuration error");
    check(error != NULL, "validation failure reports an error string");
    mahak_free(error);

    error = NULL;
    check(mahak_validate_manifest(NULL, &error) == 2, "null manifest path is rejected");
    mahak_free(error);

    check(mahak_validate_manifest("/dev/null", NULL) == 2, "null out_error is tolerated");

    mahak_engine *engine = mahak_engine_create();
    check(engine != NULL, "engine is created");

    char *json = NULL;
    error = NULL;
    check(mahak_run_manifest(engine, "/definitely/not/a/manifest.json", &json, &error) != 0,
          "missing manifest fails to run");
    check(json == NULL, "failed run does not produce JSON");
    mahak_free(error);

    check(mahak_run_manifest(NULL, "/dev/null", NULL, NULL) == 1, "null engine is rejected");

    /* Cancelling an idle engine is safe and idempotent. */
    mahak_engine_cancel(engine);
    mahak_engine_cancel(engine);
    mahak_engine_cancel(NULL);

    mahak_engine_destroy(engine);
    mahak_engine_destroy(NULL);
    mahak_free(NULL);

    if (failures != 0) {
        fprintf(stderr, "%d C ABI check(s) failed\n", failures);
        return 1;
    }
    printf("c_api tests passed\n");
    return 0;
}
