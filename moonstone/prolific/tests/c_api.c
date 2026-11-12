#include "prolific/prolific.h"

#include <assert.h>
#include <string.h>

int main(void) {
    prolific_session *session = NULL;
    uint64_t span = 0;
    char *json = NULL;
    prolific_config bad = {99, 1, 1, 1, 1, 1};
    assert(prolific_create(&bad, &session) == PROLIFIC_INVALID_INPUT && session == NULL);
    assert(prolific_last_error()[0] != 0);
    assert(prolific_create(NULL, &session) == PROLIFIC_OK);
    prolific_location loc = {"source.c", "ir:42", 7, 4};
    assert(prolific_begin(session, PROLIFIC_COMPILER, "pass", &loc, &span) == PROLIFIC_OK);
    assert(span != 0);
    assert(prolific_emit(session, PROLIFIC_VM, PROLIFIC_COUNTER, "add", 7, NULL, NULL) ==
           PROLIFIC_OK);
    assert(prolific_end(session, span) == PROLIFIC_OK);
    assert(prolific_report(session, &json) == PROLIFIC_OK);
    assert(strstr(json, "moonstone.prolific/1") != NULL && strstr(json, "ir:42") != NULL);
    assert(strstr(json, "\"complete\":true") != NULL);
    prolific_free_string(json);
    assert(prolific_record_loss(session) == PROLIFIC_OK);
    assert(prolific_emit(session, (prolific_domain)256, PROLIFIC_ERROR, "bad", 0, NULL, NULL) ==
           PROLIFIC_INVALID_INPUT);
    assert(prolific_end(session, span) == PROLIFIC_INVALID_INPUT);
    assert(prolific_begin(session, PROLIFIC_VM, NULL, NULL, &span) == PROLIFIC_INVALID_INPUT);
    assert(span == 0);
    assert(prolific_report(session, &json) == PROLIFIC_OK);
    assert(strstr(json, "\"complete\":false") != NULL);
    prolific_free_string(json);
    prolific_destroy(session);
    prolific_destroy(NULL);
    assert(prolific_report(NULL, &json) == PROLIFIC_INVALID_INPUT && json == NULL);
    prolific_free_string(NULL);
    return 0;
}
