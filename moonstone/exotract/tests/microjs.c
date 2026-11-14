/* Independent C11 consumer of the generated, single implementation TU. */
#include "mulib-microjs.h"
#include <assert.h>
#include <string.h>

static mulib_microjs_status_t echo(mulib_microjs_t *vm,
    const mulib_microjs_value_t *args, size_t count,
    mulib_microjs_value_t *out, void *user) {
    mulib_microjs_value_t nested = {0};
    mulib_microjs_error_t error = {0};
    assert(count == 1 && args[0].type == MULIB_MICROJS_STRING);
    ++*(int *)user;
    /* Reentrant execution is rejected; collection is a no-op while running. */
    assert(mulib_microjs_eval(vm, "1", 1, &nested, &error) == MULIB_MICROJS_INVALID);
    assert(nested.type == MULIB_MICROJS_NULL);
    size_t bytes = mulib_microjs_collect(vm);
    assert(bytes == mulib_microjs_collect(vm));
    /* Arguments are borrowed: produce an owned copy rather than shallow-copy. */
    return mulib_microjs_value_copy(vm, &args[0], out);
}

int main(void) {
    int calls = 0;
    mulib_microjs_t *vm = mulib_microjs_create(NULL);
    mulib_microjs_value_t string = {0}, object = {0}, copy = {0}, property = {0};
    mulib_microjs_error_t error = {0};
    assert(vm && !mulib_microjs_create_with_heap(NULL, 0));
    assert(mulib_microjs_bind(vm, "echo", echo, &calls) == MULIB_MICROJS_OK);
    const char *source = "echo('owned')";
    assert(mulib_microjs_eval(vm, source, strlen(source), &string, &error) == MULIB_MICROJS_OK);
    assert(calls == 1 && string.length == 5 && !memcmp(string.string, "owned", 5));
    assert(mulib_microjs_make_object(vm, 0, &object) == MULIB_MICROJS_OK);
    assert(mulib_microjs_set_property(vm, &object, "key", &string) == MULIB_MICROJS_OK);
    assert(mulib_microjs_value_copy(vm, &object, &copy) == MULIB_MICROJS_OK);
    mulib_microjs_value_free(&object);
    (void)mulib_microjs_collect(vm);
    assert(mulib_microjs_get_property(vm, &copy, "key", &property) == MULIB_MICROJS_OK);
    assert(property.length == 5 && !memcmp(property.string, "owned", 5));
    mulib_microjs_value_free(&property);
    assert(mulib_microjs_eval(vm, "let =", 5, &property, &error) == MULIB_MICROJS_SYNTAX);
    assert(property.type == MULIB_MICROJS_NULL && !property.string && !property.object);
    mulib_microjs_destroy(vm);
    /* Owned strings and rooted handles remain releasable after state teardown. */
    assert(string.length == 5 && !memcmp(string.string, "owned", 5));
    mulib_microjs_value_free(&string);
    mulib_microjs_value_free(&copy);
    return 0;
}
