#include "runtime.h"
#include "integer_ops.h"
#include <stdlib.h>

#define NONE SIZE_MAX

/* A closure refers to immutable syntax and a lexical binding chain. NONE as
 * node denotes a supplied or evaluated integer. All other indices are into
 * per-call arenas; reallocations cannot invalidate a closure or continuation. */
typedef struct { size_t node, env; int64_t integer; } closure;
typedef struct { closure value; size_t next; } link;
typedef struct {
  closure owner;
  size_t pending, remaining, next_child;
  int64_t first;
} frame;
typedef struct {
  const traceml_module_v1 *program;
  traceml_options options;
  link *bindings, *args;
  frame *frames;
  size_t binding_count, binding_capacity, arg_count, arg_capacity, frame_count, frame_capacity;
  size_t steps, site;
} machine;

void traceml_options_default(traceml_options *options) {
  if (options) *options = (traceml_options){100000, 1000000, NULL, NULL};
}

const char *traceml_status_message(traceml_status status) {
  switch (status) {
    case TRACEML_OK: return "success";
    case TRACEML_INVALID_ARGUMENT: return "invalid runtime argument";
    case TRACEML_INVALID_PROGRAM: return "invalid TraceML module or runtime ABI";
    case TRACEML_TYPE_ERROR: return "expected an integer or callable value";
    case TRACEML_ARITY_ERROR: return "wrong number of primitive arguments";
    case TRACEML_OVERFLOW: return "integer overflow";
    case TRACEML_LIMIT: return "TraceML step or state limit reached";
    case TRACEML_OUT_OF_MEMORY: return "TraceML runtime allocation failed";
    case TRACEML_CANCELLED: return "TraceML evaluation cancelled";
  }
  return "unknown TraceML status";
}

static int validate(machine *m) {
  const traceml_module_v1 *p = m->program;
  size_t i, j;
  if (p->abi != TRACEML_ABI_V1 || !p->node_count || !p->form_count ||
      p->node_count > UINT32_MAX || p->edge_count > UINT32_MAX || p->form_count > UINT32_MAX ||
      !p->nodes || !p->forms || (p->edge_count && !p->edges)) return 0;
  for (i = 0; i < p->node_count; ++i) {
    const traceml_node_v1 *n = p->nodes + i;
    size_t first, count;
    m->site = i;
    if (n->first_edge > p->edge_count || n->edge_count > p->edge_count - n->first_edge ||
        (n->opcode != TRACEML_LOCAL && n->lexical_index) ||
        (n->opcode != TRACEML_CONST && n->integer)) return 0;
    first = n->first_edge;
    count = n->edge_count;
    switch (n->opcode) {
      case TRACEML_LAMBDA: if (count != 1) return 0; break;
      case TRACEML_APPLY: if (count < 2) return 0; break;
      case TRACEML_IF: if (count != 3) return 0; break;
      case TRACEML_BEGIN: if (!count) return 0; break;
      case TRACEML_CONST: case TRACEML_LOCAL:
      case TRACEML_ADD: case TRACEML_SUB: case TRACEML_MUL:
      case TRACEML_EQ: case TRACEML_LT: case TRACEML_NEG:
        if (count) return 0;
        break;
      default: return 0;
    }
    for (j = 0; j < count; ++j) if (p->edges[first + j] >= i) return 0;
  }
  m->site = NONE;
  for (i = 0; i < p->form_count; ++i) if (p->forms[i] >= p->node_count) return 0;
  return 1;
}

static void *reserve_record(void *data, size_t count, size_t *capacity,
                            size_t item_size, size_t limit, traceml_status *status) {
  size_t maximum = SIZE_MAX / item_size;
  size_t next;
  void *grown;
  if (maximum > limit) maximum = limit;
  if (count >= maximum) { *status = TRACEML_LIMIT; return NULL; }
  if (count < *capacity) return data;
  next = *capacity == 0 ? 16 : (*capacity > maximum / 2 ? maximum : 2 * *capacity);
  if (next > maximum) next = maximum;
  grown = realloc(data, next * item_size);
  if (!grown) { *status = TRACEML_OUT_OF_MEMORY; return NULL; }
  *capacity = next;
  return grown;
}

static traceml_status add_link(link **arena, size_t *count, size_t *capacity,
                              size_t limit, closure value, size_t *head) {
  traceml_status status;
  link *records = reserve_record(*arena, *count, capacity, sizeof(*records), limit, &status);
  if (!records) return status;
  *arena = records;
  records[*count] = (link){value, *head};
  *head = (*count)++;
  return TRACEML_OK;
}

static traceml_status push_frame(machine *m, frame value) {
  traceml_status status;
  frame *records = reserve_record(m->frames, m->frame_count, &m->frame_capacity,
                                  sizeof(*records), m->options.state_limit, &status);
  if (!records) return status;
  m->frames = records;
  m->frames[m->frame_count++] = value;
  return TRACEML_OK;
}

static closure child(const machine *m, closure owner, size_t i) {
  return (closure){m->program->edges[(size_t)m->program->nodes[owner.node].first_edge + i], owner.env, 0};
}

static traceml_status evaluate(machine *m, closure control, size_t pending, closure *result) {
  closure value = {0, 0, 0};
  int returning = 0;
  for (;;) {
    traceml_status status;
    const traceml_node_v1 *node;
    if (returning) {
      frame *f;
      if (!m->frame_count) { *result = value; return TRACEML_OK; }
      f = &m->frames[m->frame_count - 1];
      m->site = f->owner.node;
      node = m->program->nodes + f->owner.node;
      if (node->opcode == TRACEML_IF) {
        if (value.node != NONE) return TRACEML_TYPE_ERROR;
        control = child(m, f->owner, value.integer ? 1 : 2);
        pending = f->pending;
      } else if (node->opcode == TRACEML_BEGIN) {
        control = child(m, f->owner, f->next_child++);
        if (f->next_child < node->edge_count) { returning = 0; continue; }
        pending = f->pending;
      } else {
        int64_t integer;
        if (value.node != NONE) return TRACEML_TYPE_ERROR;
        if (f->remaining != NONE) {
          f->first = value.integer;
          control = m->args[f->remaining].value;
          f->remaining = NONE;
          returning = 0;
          continue;
        }
        status = traceml_integer_op(node->opcode,
          node->opcode == TRACEML_NEG ? value.integer : f->first, value.integer, &integer);
        if (status != TRACEML_OK) return status;
        value = (closure){NONE, NONE, integer};
        --m->frame_count;
        continue;
      }
      --m->frame_count;
      returning = 0;
      continue;
    }
    if (control.node != NONE) m->site = control.node;
    if (m->options.cancelled && m->options.cancelled(m->options.userdata)) return TRACEML_CANCELLED;
    if (m->steps == m->options.step_limit) return TRACEML_LIMIT;
    ++m->steps;
    if (control.node == NONE) {
      if (pending != NONE) return TRACEML_TYPE_ERROR;
      value = control;
      returning = 1;
      continue;
    }
    node = m->program->nodes + control.node;
    switch (node->opcode) {
      case TRACEML_CONST:
        if (pending != NONE) return TRACEML_TYPE_ERROR;
        value = (closure){NONE, NONE, node->integer};
        returning = 1;
        break;
      case TRACEML_LOCAL: {
        size_t env = control.env;
        uint32_t i;
        for (i = 0; i < node->lexical_index && env != NONE; ++i) env = m->bindings[env].next;
        if (env == NONE) return TRACEML_INVALID_PROGRAM;
        control = m->bindings[env].value;
        break;
      }
      case TRACEML_LAMBDA:
        if (pending == NONE) { value = control; returning = 1; break; }
        status = add_link(&m->bindings, &m->binding_count, &m->binding_capacity,
                          m->options.state_limit, m->args[pending].value, &control.env);
        if (status != TRACEML_OK) return status;
        pending = m->args[pending].next;
        control = child(m, control, 0);
        break;
      case TRACEML_APPLY: {
        size_t i;
        for (i = node->edge_count; i > 1; --i) {
          status = add_link(&m->args, &m->arg_count, &m->arg_capacity,
                            m->options.state_limit, child(m, control, i - 1), &pending);
          if (status != TRACEML_OK) return status;
        }
        control = child(m, control, 0);
        break;
      }
      case TRACEML_IF: case TRACEML_BEGIN:
        if (node->opcode == TRACEML_IF || node->edge_count > 1) {
          status = push_frame(m, (frame){control, pending, NONE, 1, 0});
          if (status != TRACEML_OK) return status;
          pending = NONE;
        }
        control = child(m, control, 0);
        break;
      default: {
        size_t second;
        if (pending == NONE) { value = control; returning = 1; break; }
        second = m->args[pending].next;
        if (node->opcode == TRACEML_NEG ? second != NONE :
            second == NONE || m->args[second].next != NONE) return TRACEML_ARITY_ERROR;
        status = push_frame(m, (frame){control, NONE, second, 0, 0});
        if (status != TRACEML_OK) return status;
        control = m->args[pending].value;
        pending = NONE;
        break;
      }
    }
  }
}

traceml_status traceml_run_v1(const traceml_module_v1 *program,
                             const int64_t *args, size_t count, const traceml_options *options,
                             int64_t *result, traceml_error *error) {
  machine m = {0};
  traceml_status status = TRACEML_INVALID_ARGUMENT;
  closure value;
  size_t i, pending = NONE;
  m.program = program;
  m.site = NONE;
  if (!program || !result || (count && !args)) goto done;
  if (options) m.options = *options;
  else traceml_options_default(&m.options);
  if (!validate(&m)) { status = TRACEML_INVALID_PROGRAM; goto done; }
  if (count > m.options.state_limit) { status = TRACEML_LIMIT; goto done; }
  for (i = 0; i < program->form_count; ++i) {
    if (i == program->form_count - 1) {
      size_t j;
      for (j = count; j > 0; --j) {
        status = add_link(&m.args, &m.arg_count, &m.arg_capacity, m.options.state_limit,
                          (closure){NONE, NONE, args[j - 1]}, &pending);
        if (status != TRACEML_OK) goto done;
      }
    }
    status = evaluate(&m, (closure){program->forms[i], NONE, 0}, pending, &value);
    if (status != TRACEML_OK) goto done;
    /* Top-level forms cannot share lexical bindings or argument closures. */
    m.binding_count = m.arg_count = 0;
  }
  if (value.node != NONE) { status = TRACEML_TYPE_ERROR; goto done; }
  *result = value.integer;
done:
  if (error) {
    *error = (traceml_error){status, 0, 0, 0, m.steps};
    if (status != TRACEML_OK && m.site != NONE) {
      const traceml_node_v1 *n = program->nodes + m.site;
      error->offset = n->offset;
      error->line = n->line;
      error->column = n->column;
    }
  }
  free(m.bindings);
  free(m.args);
  free(m.frames);
  return status;
}
