#include "Flintstone.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "C API failure at %d: %s (%s)\n", __LINE__, #c, flintstone_last_error()); return 1; } } while (0)
int header_other(void);
int main(void) {
    const char* source = "module c level high {func @f(%x:i32)->i32 {^e:%r:i32=math.madd(%x,3:i32,12:i32);func.return(%r);}}";
    flintstone_context* context = flintstone_context_create();
    flintstone_module* module = NULL;
    int64_t argument = 10, answer = -1;
    const char* text = NULL;
    CHECK(context && header_other());
    CHECK(flintstone_parse(context, source, strlen(source), "c.flint", &module) == FLINTSTONE_OK && module);
    CHECK(flintstone_verify(context, module) == FLINTSTONE_OK);
    CHECK(flintstone_run_i64(context, module, "f", &argument, 1, 1000, &answer) == FLINTSTONE_OK && answer == 42);
    CHECK(flintstone_lower(context, module, FLINTSTONE_CORE) == FLINTSTONE_OK);
    CHECK(flintstone_print(module, &text) == FLINTSTONE_OK && text && strstr(text, "arith.mul"));
    CHECK(flintstone_lower(context, module, (flintstone_level)99) == FLINTSTONE_INVALID_ARGUMENT);
    CHECK(flintstone_run_i64(context, module, "f", &argument, 1, 0, &answer) == FLINTSTONE_ERROR);
    CHECK(strstr(flintstone_last_error(), "budget"));
    CHECK(flintstone_run_i64(context, module, "f", NULL, 1, 1000, &answer) == FLINTSTONE_INVALID_ARGUMENT);
    CHECK(flintstone_binding_target_count() == 9);
    CHECK(flintstone_binding_target_at(0) != NULL && flintstone_binding_target_at(999) == NULL);
    flintstone_context_destroy(context);
    CHECK(flintstone_module_text(module) != NULL); /* Module outlives context. */
    flintstone_module_destroy(module);
    context = flintstone_context_create();
    module = flintstone_parse_text(context,
        "module imp level core { import \"math\"; func @f(%x:i32)->i32 {^e:%r:i32=func.call(%x) attrs {callee=\"std.math.factorial_i32\";};func.return(%r);}}");
    CHECK(module != NULL);
    CHECK(flintstone_verify(context, module) == FLINTSTONE_ERROR); /* unresolved */
    CHECK(flintstone_resolve_imports(context, module, NULL) == FLINTSTONE_OK);
    CHECK(flintstone_verify(context, module) == FLINTSTONE_OK);
    argument = 5;
    CHECK(flintstone_run_i64(context, module, "f", &argument, 1, 10000, &answer) == FLINTSTONE_OK && answer == 120);
    CHECK(flintstone_resolve_imports(NULL, module, NULL) == FLINTSTONE_INVALID_ARGUMENT);
    flintstone_module_destroy(module);
    module = flintstone_parse_text(context,
        "module opt level core {func @f(%x:i32)->i32 {^e:%r:i32=arith.add(%x,0:i32);func.return(%r);}}");
    CHECK(module != NULL);
    CHECK(flintstone_optimize(context, module, BLACKSTONE_KERNEL_DIR, "add_zero") == FLINTSTONE_OK);
    argument = 10;
    CHECK(flintstone_run_i64(context, module, "f", &argument, 1, 1000, &answer) == FLINTSTONE_OK && answer == 10);
    CHECK(flintstone_optimize(context, module, BLACKSTONE_KERNEL_DIR, "no_such_kernel") == FLINTSTONE_ERROR);
    CHECK(flintstone_optimize(context, module, "/no/such/dir", "add_zero") == FLINTSTONE_ERROR);
    CHECK(flintstone_optimize(NULL, module, BLACKSTONE_KERNEL_DIR, "add_zero") == FLINTSTONE_INVALID_ARGUMENT);
    flintstone_module_destroy(module);
    module = flintstone_parse_text(context, "invalid");
    CHECK(!module && strlen(flintstone_last_error()) > 0);
    CHECK(flintstone_verify(NULL, NULL) == FLINTSTONE_INVALID_ARGUMENT);
    flintstone_module_destroy(NULL); flintstone_context_destroy(context); flintstone_context_destroy(NULL);
    return 0;
}
