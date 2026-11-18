#ifndef TRACEML_RUNTIME_H
#define TRACEML_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum traceml_status {
  TRACEML_OK = 0,
  TRACEML_INVALID_ARGUMENT = 1, TRACEML_INVALID_PROGRAM = 2, TRACEML_TYPE_ERROR = 3,
  TRACEML_ARITY_ERROR = 4, TRACEML_OVERFLOW = 5, TRACEML_LIMIT = 6,
  TRACEML_OUT_OF_MEMORY = 7, TRACEML_CANCELLED = 8
} traceml_status;

typedef struct traceml_options {
  size_t step_limit;  /* Source-level steps; zero permits no evaluation. */
  size_t state_limit; /* Maximum records in each transient state arena. */
  int (*cancelled)(void *); /* Synchronous, called before each source step. */
  void *userdata;
} traceml_options;

typedef struct traceml_error {
  traceml_status status;
  uint32_t offset, line, column; /* Zero line/column when no source site applies. */
  size_t steps;
} traceml_error;

void traceml_options_default(traceml_options *options);
const char *traceml_status_message(traceml_status status);

/* Declare a generated entry with: extern traceml_entry_v1 traceml_generated_name_run;
 * Arguments are applied to the final form; earlier forms execute in order.
 * The entry borrows arguments, accepts NULL options for defaults and optional
 * diagnostics, and writes the required result only on success. Calls are isolated. */
typedef traceml_status traceml_entry_v1(const int64_t *args, size_t count,
  const traceml_options *options, int64_t *result, traceml_error *error);

/* Versioned compiler/runtime interface. Tables are immutable and remain
 * borrowed for the call. All indices in a node's edge range must precede it. */
#define TRACEML_ABI_V1 1
enum traceml_opcode_v1 {
  TRACEML_CONST = 0, TRACEML_LOCAL = 1, TRACEML_LAMBDA = 2, TRACEML_APPLY = 3,
  TRACEML_IF = 4, TRACEML_BEGIN = 5, TRACEML_ADD = 6, TRACEML_SUB = 7, TRACEML_MUL = 8,
  TRACEML_EQ = 9, TRACEML_LT = 10, TRACEML_NEG = 11
};
typedef struct traceml_node_v1 {
  uint32_t opcode, first_edge, edge_count, lexical_index;
  int64_t integer;
  uint32_t offset, line, column;
} traceml_node_v1;
typedef struct traceml_module_v1 {
  uint32_t abi;
  const traceml_node_v1 *nodes;
  size_t node_count;
  const uint32_t *edges;
  size_t edge_count;
  const uint32_t *forms;
  size_t form_count;
} traceml_module_v1;
traceml_status traceml_run_v1(const traceml_module_v1 *, const int64_t *args, size_t count,
  const traceml_options *, int64_t *result, traceml_error *);

#ifdef __cplusplus
}
#endif
#endif
