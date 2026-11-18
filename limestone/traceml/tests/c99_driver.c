#include <traceml/runtime.h>
#include <stdio.h>
#include <stdlib.h>

#define ENTRY(name) extern traceml_entry_v1 traceml_generated_##name##_run
ENTRY(calc);
ENTRY(shadow);
ENTRY(closure);
ENTRY(twice);
ENTRY(diverge);
ENTRY(overflow);
ENTRY(earlier);
ENTRY(constant);
ENTRY(arity);
ENTRY(type_error);
ENTRY(left_first);
ENTRY(callable);

#define CHECK(condition) do { \
  if (!(condition)) { fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return EXIT_FAILURE; } \
} while (0)

static int cancel(void *value) {
  return ++*(unsigned *)value == 13;
}

int main(void) {
  int64_t arg = 40, result = 0;
  traceml_error error;
  traceml_options options;
  unsigned callbacks = 0;
  traceml_options_default(&options);

  CHECK(traceml_generated_calc_run(&arg, 1, NULL, &result, &error) == TRACEML_OK);
  CHECK(result == 42 && error.status == TRACEML_OK && error.steps > 1);
  arg = -7;
  CHECK(traceml_generated_calc_run(&arg, 1, NULL, &result, &error) == TRACEML_OK && result == 7);
  CHECK(traceml_generated_shadow_run(NULL, 0, NULL, &result, NULL) == TRACEML_OK && result == 42);
  CHECK(traceml_generated_closure_run(NULL, 0, NULL, &result, NULL) == TRACEML_OK && result == 42);
  CHECK(traceml_generated_twice_run(NULL, 0, NULL, &result, &error) == TRACEML_OK);
  CHECK(result == 42 && error.steps == 14); /* Both uses force the argument separately. */
  CHECK(traceml_generated_constant_run(NULL, 0, NULL, &result, NULL) == TRACEML_OK && result == INT64_MIN);
  result = 99;
  CHECK(traceml_generated_overflow_run(NULL, 0, NULL, &result, &error) == TRACEML_OVERFLOW);
  CHECK(result == 99 && error.status == TRACEML_OVERFLOW && error.line == 2 && error.column == 4);
  CHECK(traceml_generated_earlier_run(&arg, 1, NULL, &result, &error) == TRACEML_OVERFLOW && error.line == 1);
  CHECK(traceml_generated_arity_run(NULL, 0, NULL, &result, &error) == TRACEML_ARITY_ERROR);
  CHECK(traceml_generated_type_error_run(NULL, 0, NULL, &result, &error) == TRACEML_TYPE_ERROR);
  CHECK(traceml_generated_left_first_run(NULL, 0, NULL, &result, &error) == TRACEML_OVERFLOW);
  CHECK(traceml_generated_callable_run(NULL, 0, NULL, &result, &error) == TRACEML_TYPE_ERROR && result == 99);
  CHECK(traceml_generated_calc_run(NULL, 1, NULL, &result, &error) == TRACEML_INVALID_ARGUMENT);

  options.step_limit = 10000;
  CHECK(traceml_generated_diverge_run(NULL, 0, &options, &result, &error) == TRACEML_LIMIT);
  CHECK(result == 99 && error.steps == 10000);
  options.step_limit = 0;
  CHECK(traceml_generated_constant_run(NULL, 0, &options, &result, &error) == TRACEML_LIMIT && error.steps == 0);
  traceml_options_default(&options);
  options.cancelled = cancel;
  options.userdata = &callbacks;
  CHECK(traceml_generated_diverge_run(NULL, 0, &options, &result, &error) == TRACEML_CANCELLED);
  CHECK(result == 99 && callbacks == 13 && error.steps == 12);
  traceml_options_default(&options);
  options.state_limit = 1;
  CHECK(traceml_generated_diverge_run(NULL, 0, &options, &result, &error) == TRACEML_LIMIT);
  CHECK(traceml_generated_constant_run(NULL, 0, &options, &result, &error) == TRACEML_OK);

  {
    uint32_t self = 0, root = 0;
    traceml_node_v1 node = {TRACEML_LAMBDA, 0, 1, 0, 0, 0, 5, 6};
    traceml_module_v1 malformed = {TRACEML_ABI_V1, &node, 1, &self, 1, &root, 1};
    CHECK(traceml_run_v1(&malformed, NULL, 0, NULL, &result, &error) == TRACEML_INVALID_PROGRAM);
    CHECK(error.line == 5 && error.column == 6);
    malformed.abi = 0;
    CHECK(traceml_run_v1(&malformed, NULL, 0, NULL, &result, &error) == TRACEML_INVALID_PROGRAM && error.line == 0);
  }
  return EXIT_SUCCESS;
}
