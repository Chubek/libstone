/* ==========================================================================
 * MemTkX.i -- SWIG 4.0 interface for the MemTKX C library (v2.x)
 *
 * Wraps the C99 API (<memtkx/memtkx.h>), not the C++ wrapper. Works with
 * any SWIG target language that supports structs, enums and typemaps.i
 * (Python, Ruby, Perl, PHP, Tcl, Lua, Java, C#, Go, ...).
 *
 *   swig -python -I<prefix>/include -outdir . MemTkX.i
 *   cc -shared -fPIC MemTkX_wrap.c -I<prefix>/include \
 *      $(python3-config --includes) -L<prefix>/lib -lmemtkx -pthread \
 *      -o _MemTkX$(python3-config --extension-suffix)
 *
 * Binding-specific behaviour (read this):
 *
 *  - Lifetimes. Objects created by *_create / *_new / constructors are
 *    owned by the target-language proxy and released by its destructor.
 *    Calling the matching *_destroy / *_free / *_release explicitly is also
 *    safe: those functions are marked %delobject and disown the proxy.
 *    Pointers returned by accessors (`space.base`, `result.value`,
 *    mtkx_ast_child, ...) are borrowed: keep their owner alive. Derived
 *    PEG definitions, matchers and rule parsers reference their
 *    definition, which must outlive them.
 *
 *  - Text. SWIG passes strings as temporary copies. APIs that keep a
 *    pointer to caller text are wrapped so the binding owns a copy:
 *    mtkx_peg_matcher_create, mtkx_run_parser (match spans in the result)
 *    and the mtkx_parse_input constructor. Functions that only borrow a
 *    string until a later call (mtkx_make_phase, mtkx_task_make,
 *    mtkx_thread_record_init, mtkx_when_pattern, ...) are replaced by the
 *    safe helpers documented below.
 *
 *  - Nothing consumes its arguments here. Parser combinators take their
 *    own reference (in C they consume the caller's), and
 *    mtkx_ast_append_child / mtkx_task_state_set store copies.
 *
 *  - Callbacks are C function pointers. Pass None/null for "no callback",
 *    one of the built-in C callbacks exported below
 *    (MTKX_FIELD_TRACE, MTKX_VEC_ROOT_PROVIDER), or a pointer from your
 *    language's native FFI. Writing callbacks in the target language needs
 *    language-specific typemaps and is out of scope for this file.
 *
 *  - *_unwrap / *_unwrap_err on the wrong branch raise ValueError (or the
 *    language's equivalent) instead of reaching mtkx_panic().
 *
 *  - Raw memory. mtkx_buffer_alloc/free and mtkx_peek_* and mtkx_poke_* let
 *    scripts provide borrowed storage and read/write object fields. They
 *    perform no checks: wrong addresses crash the process, as in C.
 *
 *  - Threads. The coordinator is thread-safe; nothing else is. For Python,
 *    add `-threads` to release the GIL in blocking calls such as
 *    mtkx_coordinator_request_stop_the_world / _poll_safepoint.
 * ========================================================================== */

%module(docstring="MemTKX: C99 memory-management toolkit (allocators, spaces, barriers, collection plans, coordination and DSL utilities).") MemTkX

%feature("autodoc", "1");

%{
#include <stdlib.h>
#include <string.h>

#include "memtkx/memtkx.h"

/* ---- binding-owned text registry (owner pointer -> owned copy) ------- */
typedef struct mtkx_swig_text_ {
  const void *owner;
  char *text;
  struct mtkx_swig_text_ *next;
} mtkx_swig_text_;

static mtkx_swig_text_ *mtkx_swig_texts_ = NULL;

/* helpers defined in %inline below, referenced earlier by %extend code */
mtkx_status mtkx_vec_root_provider(mtkx_address_vec *roots, void *ctx);

static void mtkx_swig_attach_text_(const void *owner, char *text) {
  mtkx_swig_text_ *node;
  if (owner == NULL || text == NULL) {
    mtkx_free(text);
    return;
  }
  node = (mtkx_swig_text_ *)malloc(sizeof *node);
  if (node == NULL) {
    return; /* leak the copy rather than leave a dangling pointer */
  }
  node->owner = owner;
  node->text = text;
  node->next = mtkx_swig_texts_;
  mtkx_swig_texts_ = node;
}

static void mtkx_swig_release_text_(const void *owner) {
  mtkx_swig_text_ **link = &mtkx_swig_texts_;
  if (owner == NULL) {
    return;
  }
  while (*link != NULL) {
    mtkx_swig_text_ *node = *link;
    if (node->owner == owner) {
      *link = node->next;
      mtkx_free(node->text);
      free(node);
    } else {
      link = &node->next;
    }
  }
}
%}

/* ==========================================================================
 *  Typemaps and target-language adjustments
 * ========================================================================== */

%include "typemaps.i"
%include "exception.i"

#if defined(SWIGRUBY)
#pragma SWIG nowarn=801   /* Ruby capitalises class names; harmless */
/* SWIG/Ruby defines Type_allocate for every wrapped struct, which clashes
 * with functions such as mtkx_space_allocate: Ruby spells those *_alloc. */
%define MTKX_SWIG_RUBY_ALLOC(Type)
%rename(Type##_alloc) Type##_allocate;
%enddef
MTKX_SWIG_RUBY_ALLOC(mtkx_space)
MTKX_SWIG_RUBY_ALLOC(mtkx_bump_allocator)
MTKX_SWIG_RUBY_ALLOC(mtkx_bump_space)
MTKX_SWIG_RUBY_ALLOC(mtkx_free_list_allocator)
MTKX_SWIG_RUBY_ALLOC(mtkx_free_list_space)
MTKX_SWIG_RUBY_ALLOC(mtkx_segregated_allocator)
MTKX_SWIG_RUBY_ALLOC(mtkx_immix_space)
MTKX_SWIG_RUBY_ALLOC(mtkx_los_space)
MTKX_SWIG_RUBY_ALLOC(mtkx_plan)
MTKX_SWIG_RUBY_ALLOC(mtkx_mark_sweep_plan)
MTKX_SWIG_RUBY_ALLOC(mtkx_semi_space_plan)
MTKX_SWIG_RUBY_ALLOC(mtkx_immix_plan)
MTKX_SWIG_RUBY_ALLOC(mtkx_generational_plan)
#endif

/* Platform integer typedefs are left opaque to SWIG and mapped onto wide
 * builtin typemaps, so the generated C keeps the real types on every ABI. */
%apply unsigned long long { uintptr_t, uint64_t };
%apply long long { intmax_t, ptrdiff_t };
%apply unsigned char { uint8_t };

/* Strings returned by MemTKX that the caller must release. */
%typemap(newfree) char * "mtkx_free($1);";

/* Length-delimited text arguments: pass one string. Target languages without
 * the (STRING, LENGTH) multi-argument typemap take the text and its length
 * as two separate arguments. */
#if defined(SWIGPYTHON) || defined(SWIGRUBY) || defined(SWIGPERL) || defined(SWIGTCL) || \
    defined(SWIGJAVA) || defined(SWIGGO) || defined(SWIGPHP) || defined(SWIGJAVASCRIPT) || \
    defined(SWIGOCTAVE) || defined(SWIGR) || defined(SWIGGUILE) || defined(SWIGOCAML)
%apply (char *STRING, size_t LENGTH) {
  (const char *text, size_t length),
  (const char *source, size_t length),
  (const char *input, size_t input_length),
  (const char *pattern, size_t pattern_length),
  (const char *key, size_t length)
};
#endif

/* Simple out-parameters become extra return values. */
%apply int *OUTPUT { int *status_out };

/* Struct members are read-only; the few fields scripts are meant to change
 * get setter methods at the end of this file. */
%immutable;

/* ==========================================================================
 *  Things that cannot or should not be wrapped as-is
 * ========================================================================== */

/* internals */
%ignore mtkx_max_align_u_;
%ignore mtkx_max_align_probe_;
%ignore mtkx_expr_binary_;
%ignore mtkx_barrier_stage_make_;
%ignore MTKX_ADDRESS_VEC_INIT;
%ignore MTKX_GENERATED_;

/* variadic / va_list / long double / pointer-to-pointer signatures */
%ignore mtkx_ast_leaf_fmt;
%ignore mtkx_ast_node_of;
%ignore mtkx_ast_node_ofv;
%ignore mtkx_ast_node_new;       /* use mtkx_ast_inner + mtkx_ast_append_child */
%ignore mtkx_ast_append_child;   /* replaced (appends a copy) */
%ignore mtkx_peg_seq;            /* use mtkx_peg_seq2 / mtkx_peg_seq3 */
%ignore mtkx_peg_choice;         /* use mtkx_peg_choice2 / mtkx_peg_choice3 */
%ignore mtkx_units_parse_literal;      /* replaced (double result) */
%ignore mtkx_literal_set_parse;        /* long double results */
%ignore mtkx_literal_set_parse_n;
%ignore mtkx_lit;
%ignore mtkx_literal_set_make;
%ignore mtkx_literal_entry;
%ignore mtkx_literal_set;
%ignore mtkx_load_parse_input_file;    /* replaced */
%ignore mtkx_peg_definition_add_rule;  /* replaced */
%ignore mtkx_immix_block_allocate;     /* replaced (returns the address, 0 on failure) */
%ignore mtkx_peg_rule_try_match;       /* replaced (returns the end, -1 on failure) */

/* functions that keep a pointer to caller text after returning */
%ignore mtkx_parse_input_make;         /* use the mtkx_parse_input constructor */
%ignore mtkx_strview_make;
%ignore mtkx_strview_cstr;
%ignore mtkx_make_phase;               /* use mtkx_task_pipeline_append_phase */
%ignore mtkx_task_make;                /* use mtkx_task_chain_append_task */
%ignore mtkx_thread_record_init;       /* use the mtkx_thread_record constructor */
%ignore mtkx_when_pattern;
%ignore mtkx_production_make;
%ignore mtkx_production_find;
%ignore mtkx_peg_channel_arm;          /* use mtkx_peg_matcher_feed_channel */

/* ownership moves that a garbage-collected proxy cannot express */
%ignore mtkx_parse_result_take_value;
%ignore mtkx_parse_fail_with;
%ignore mtkx_parse_ok;                 /* building parse values needs a C parser */
%ignore mtkx_parse_value_char;
%ignore mtkx_parse_value_bool;
%ignore mtkx_parse_value_pair;
%ignore mtkx_parse_value_list;
%ignore mtkx_parse_value_list_push;
%ignore mtkx_parse_value_none;
%ignore mtkx_parse_value_some;
%ignore mtkx_parse_value_match;
%ignore mtkx_parse_value_user;
%ignore mtkx_parse_value_free;         /* values are owned by their result */
%ignore mtkx_task_state_set;           /* replaced (stores a copy) */
%ignore mtkx_parser_new;               /* needs a C ctx_free */

/* raw arrays and pointers inside structs (accessors are added below) */
%ignore mtkx_address_vec::data;
%ignore mtkx_slot_buffer::data;
%ignore mtkx_strview::data;
%ignore mtkx_match_key::text;
%ignore mtkx_parse_value::as;
%rename(as_) as;                  /* keyword in several target languages */
%ignore mtkx_parse_error::expected;
%ignore mtkx_parse_input::source;
%ignore mtkx_immix_block::line_marks;
%ignore mtkx_immix_space::storage;
%ignore mtkx_immix_space::blocks;
%ignore mtkx_segregated_allocator::bins;
%ignore mtkx_segregated_allocator::ranges;
%ignore mtkx_free_list_allocator::cells;
%ignore mtkx_card_table::cards;
%ignore mtkx_task_state::records;
%ignore mtkx_task_chain::tasks;
%ignore mtkx_task_pipeline::phases;
%ignore mtkx_work_queue::data;
%ignore mtkx_memo::slots;
%ignore mtkx_peg_rule::compiled;
%ignore mtkx_segregated_size_classes;  /* use mtkx_segregated_size_class(i) */
%rename(self_) mtkx_expr::self;        /* keyword in several target languages */

/* Safe replacements defined in the %inline section at the end take the
 * names of the C functions they replace. These must precede the headers. */
%rename(mtkx_units_parse_literal) mtkx_swig_units_parse_literal;
%rename(mtkx_peg_definition_add_rule) mtkx_swig_peg_definition_add_rule;
#if defined(SWIGRUBY)
%rename(mtkx_immix_block_alloc) mtkx_swig_immix_block_allocate;
#else
%rename(mtkx_immix_block_allocate) mtkx_swig_immix_block_allocate;
#endif
%rename(mtkx_peg_rule_try_match) mtkx_swig_peg_rule_try_match;
%rename(mtkx_ast_append_child) mtkx_swig_ast_append_child;
%rename(mtkx_task_state_set) mtkx_swig_task_state_set;
%rename(mtkx_peg_matcher_create) mtkx_swig_peg_matcher_create;
%rename(mtkx_peg_matcher_destroy) mtkx_swig_peg_matcher_destroy;
%rename(mtkx_run_parser) mtkx_swig_run_parser;
%rename(mtkx_parser_seq) mtkx_swig_parser_seq;
%rename(mtkx_parser_alt) mtkx_swig_parser_alt;
%rename(mtkx_parser_many) mtkx_swig_parser_many;
%rename(mtkx_parser_optional) mtkx_swig_parser_optional;
%rename(mtkx_parser_try) mtkx_swig_parser_try;
%rename(mtkx_parser_labeled) mtkx_swig_parser_labeled;
%rename(mtkx_peg_opt) mtkx_swig_peg_opt;
%rename(mtkx_peg_many) mtkx_swig_peg_many;
%rename(mtkx_peg_many1) mtkx_swig_peg_many1;
%rename(mtkx_peg_and) mtkx_swig_peg_and;
%rename(mtkx_peg_not) mtkx_swig_peg_not;
%rename(mtkx_peg_channel) mtkx_swig_peg_channel;
%rename(mtkx_peg_as_span) mtkx_swig_peg_as_span;
%ignore mtkx_peg_matcher_create;
%ignore mtkx_peg_matcher_destroy;
%ignore mtkx_run_parser;
%ignore mtkx_parser_seq;
%ignore mtkx_parser_alt;
%ignore mtkx_parser_many;
%ignore mtkx_parser_optional;
%ignore mtkx_parser_try;
%ignore mtkx_parser_labeled;
%ignore mtkx_peg_opt;
%ignore mtkx_peg_many;
%ignore mtkx_peg_many1;
%ignore mtkx_peg_and;
%ignore mtkx_peg_not;
%ignore mtkx_peg_channel;
%ignore mtkx_peg_as_span;

%newobject mtkx_swig_peg_matcher_create;
%delobject mtkx_swig_peg_matcher_destroy;
%newobject mtkx_swig_parser_seq;
%newobject mtkx_swig_parser_alt;
%newobject mtkx_swig_parser_many;
%newobject mtkx_swig_parser_optional;
%newobject mtkx_swig_parser_try;
%newobject mtkx_swig_parser_labeled;
%newobject mtkx_swig_peg_opt;
%newobject mtkx_swig_peg_many;
%newobject mtkx_swig_peg_many1;
%newobject mtkx_swig_peg_and;
%newobject mtkx_swig_peg_not;
%newobject mtkx_swig_peg_channel;
%newobject mtkx_swig_peg_as_span;

/* ==========================================================================
 *  Ownership annotations
 * ========================================================================== */

%newobject mtkx_strdup;
%newobject mtkx_strndup;
%newobject mtkx_ast_dump;
%newobject mtkx_strview::str;
%newobject mtkx_load_file;

/* plans */
%newobject mtkx_mark_sweep_plan_create;   %delobject mtkx_mark_sweep_plan_destroy;
%newobject mtkx_semi_space_plan_create;   %delobject mtkx_semi_space_plan_destroy;
%newobject mtkx_immix_plan_create;        %delobject mtkx_immix_plan_destroy;
%newobject mtkx_generational_plan_create; %delobject mtkx_generational_plan_destroy;
/* mtkx_plan_destroy destroys through a borrowed base pointer: do not use it
 * on a plan whose concrete proxy is still alive. */
%delobject mtkx_plan_destroy;

/* coordination */
%newobject mtkx_coordinator_create;       %delobject mtkx_coordinator_destroy;

/* AST */
%newobject mtkx_ast_empty;
%newobject mtkx_ast_leaf;
%newobject mtkx_ast_leaf_int;
%newobject mtkx_ast_leaf_uint;
%newobject mtkx_ast_leaf_double;
%newobject mtkx_ast_inner;
%newobject mtkx_ast_clone;
%newobject mtkx_ast_take_child;
%newobject mtkx_rewrite_apply;
%delobject mtkx_ast_free;

/* parsers (reference counted) */
%newobject mtkx_parser_retain;
%newobject mtkx_parser_ch;
%newobject mtkx_parser_satisfy;
%newobject mtkx_peg_seq2;
%newobject mtkx_peg_seq3;
%newobject mtkx_peg_choice2;
%newobject mtkx_peg_choice3;
%newobject mtkx_peg_rule_parser;
%delobject mtkx_parser_release;

/* PEG */
%newobject mtkx_peg_definition_create;
%newobject mtkx_peg_definition_derive;
%delobject mtkx_peg_definition_destroy;

/* ==========================================================================
 *  Result guards: unwrapping the wrong branch raises instead of aborting
 * ========================================================================== */

/* The pointer-out accessors Name_get / Name_get_err are hidden: test with
 * Name_is_ok / Name_has_value, then unwrap. */
%define MTKX_SWIG_GUARD_RESULT(Name)
%ignore Name##_get;
%ignore Name##_get_err;
%exception Name##_unwrap {
  if (!arg1.ok) {
    SWIG_exception(SWIG_ValueError, #Name "_unwrap called on Err");
  }
  $action
}
%exception Name##_unwrap_err {
  if (arg1.ok) {
    SWIG_exception(SWIG_ValueError, #Name "_unwrap_err called on Ok");
  }
  $action
}
%enddef

%define MTKX_SWIG_GUARD_MAYBE(Name)
%ignore Name##_get;
%exception Name##_unwrap {
  if (!arg1.has_value) {
    SWIG_exception(SWIG_ValueError, #Name "_unwrap called on an empty value");
  }
  $action
}
%enddef

MTKX_SWIG_GUARD_RESULT(mtkx_alloc_result)
MTKX_SWIG_GUARD_RESULT(mtkx_gc_result)
MTKX_SWIG_GUARD_RESULT(mtkx_plan_stats_result)
MTKX_SWIG_GUARD_MAYBE(mtkx_maybe_int)
MTKX_SWIG_GUARD_MAYBE(mtkx_maybe_long)
MTKX_SWIG_GUARD_MAYBE(mtkx_maybe_size)
MTKX_SWIG_GUARD_MAYBE(mtkx_maybe_uintptr)
MTKX_SWIG_GUARD_MAYBE(mtkx_maybe_double)
MTKX_SWIG_GUARD_MAYBE(mtkx_maybe_char)

/* ==========================================================================
 *  Opaque handles: give SWIG an (empty) definition so proxies get
 *  constructors and destructors.
 * ========================================================================== */

%nodefaultctor;
%nodefaultdtor;
struct mtkx_mark_sweep_plan {};
struct mtkx_semi_space_plan {};
struct mtkx_immix_plan {};
struct mtkx_generational_plan {};
struct mtkx_coordinator {};
struct mtkx_ast_node {};
struct mtkx_parser {};
struct mtkx_peg_definition {};
struct mtkx_peg_matcher {};
struct mtkx_peg_pattern {};
%clearnodefaultctor;
%clearnodefaultdtor;

/* ==========================================================================
 *  The C headers, in dependency order
 * ========================================================================== */

%include "memtkx/core/version.h"
%include "memtkx/core/base.h"
%include "memtkx/core/types.h"
%include "memtkx/core/vec.h"
%include "memtkx/dsl/result.h"
%include "memtkx/core/error.h"
%include "memtkx/core/units.h"

%include "memtkx/space/space.h"
%include "memtkx/space/bump_pointer.h"
%include "memtkx/space/free_list.h"
%include "memtkx/space/immix_space.h"
%include "memtkx/space/los_space.h"

%include "memtkx/barrier/barrier.h"
%include "memtkx/barrier/card_table.h"
%include "memtkx/barrier/satb.h"

%include "memtkx/scheduler/work_packet.h"
%include "memtkx/scheduler/task_pipeline.h"
%include "memtkx/scheduler/coordinator.h"

%include "memtkx/plan/plan.h"
%include "memtkx/plan/marksweep.h"
%include "memtkx/plan/semispace.h"
%include "memtkx/plan/immix.h"
%include "memtkx/plan/generational.h"

%include "memtkx/dsl/features.h"
%include "memtkx/dsl/pipeline.h"
%include "memtkx/dsl/match.h"
%include "memtkx/dsl/ast.h"
%include "memtkx/dsl/expr.h"
%include "memtkx/dsl/memo.h"
%include "memtkx/dsl/parser.h"
%include "memtkx/dsl/peg.h"
%include "memtkx/dsl/task.h"

%{
static const size_t mtkx_swig_max_align_ = MTKX_MAX_ALIGN;
%}
#undef MTKX_MAX_ALIGN
%constant size_t MTKX_MAX_ALIGN = mtkx_swig_max_align_;

/* ==========================================================================
 *  Proxy lifetimes for value structs: constructors call *_init, destructors
 *  call the (idempotent) *_destroy and then free the proxy's memory.
 * ========================================================================== */

%define MTKX_SWIG_PLAIN_DTOR(Type, Destroy)
%extend Type {
  ~Type() {
    Destroy($self);
    free($self);
  }
}
%enddef

%extend mtkx_address_vec {
  mtkx_address_vec() {
    return (mtkx_address_vec *)calloc(1, sizeof(mtkx_address_vec));
  }
  ~mtkx_address_vec() {
    mtkx_address_vec_destroy($self);
    free($self);
  }
  /** Element @p i (0 when out of range). */
  mtkx_address get(size_t i) const {
    return i < $self->size ? $self->data[i] : 0;
  }
  /** Overwrites element @p i; false when out of range. */
  bool set(size_t i, mtkx_address value) {
    if (i >= $self->size) {
      return false;
    }
    $self->data[i] = value;
    return true;
  }
}

%extend mtkx_strview {
  /** A copy of the viewed bytes. */
  char *str() const { return mtkx_strndup($self->data, $self->length); }
}

%extend mtkx_bump_allocator {
  mtkx_bump_allocator(mtkx_address start, mtkx_address end) {
    mtkx_bump_allocator *a = (mtkx_bump_allocator *)calloc(1, sizeof *a);
    if (a != NULL) {
      mtkx_bump_allocator_init(a, start, end);
    }
    return a;
  }
  ~mtkx_bump_allocator() { free($self); }
}

%extend mtkx_bump_space {
  mtkx_bump_space(const char *name, mtkx_address start, mtkx_address end) {
    mtkx_bump_space *s = (mtkx_bump_space *)calloc(1, sizeof *s);
    if (s != NULL && mtkx_bump_space_init(s, name, start, end) != MTKX_OK) {
      free(s);
      s = NULL;
    }
    return s;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_bump_space, mtkx_bump_space_destroy)

%extend mtkx_free_list_allocator {
  mtkx_free_list_allocator(mtkx_address start, mtkx_address end) {
    mtkx_free_list_allocator *a = (mtkx_free_list_allocator *)calloc(1, sizeof *a);
    if (a != NULL && mtkx_free_list_allocator_init(a, start, end) != MTKX_OK) {
      mtkx_free_list_allocator_destroy(a);
      free(a);
      a = NULL;
    }
    return a;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_free_list_allocator, mtkx_free_list_allocator_destroy)

%extend mtkx_free_list_space {
  mtkx_free_list_space(const char *name, mtkx_address start, mtkx_address end) {
    mtkx_free_list_space *s = (mtkx_free_list_space *)calloc(1, sizeof *s);
    if (s != NULL && mtkx_free_list_space_init(s, name, start, end) != MTKX_OK) {
      free(s);
      s = NULL;
    }
    return s;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_free_list_space, mtkx_free_list_space_destroy)

%extend mtkx_segregated_allocator {
  mtkx_segregated_allocator(mtkx_address start, mtkx_address end) {
    mtkx_segregated_allocator *a = (mtkx_segregated_allocator *)calloc(1, sizeof *a);
    if (a != NULL && mtkx_segregated_allocator_init(a, start, end) != MTKX_OK) {
      mtkx_segregated_allocator_destroy(a);
      free(a);
      a = NULL;
    }
    return a;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_segregated_allocator, mtkx_segregated_allocator_destroy)

%extend mtkx_immix_space {
  mtkx_immix_space(size_t bytes) {
    mtkx_immix_space *s = (mtkx_immix_space *)calloc(1, sizeof *s);
    if (s != NULL && mtkx_immix_space_init(s, bytes) != MTKX_OK) {
      free(s);
      s = NULL;
    }
    return s;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_immix_space, mtkx_immix_space_destroy)

%extend mtkx_immix_block {
  bool line_marked(size_t line) const {
    return line < MTKX_IMMIX_LINES_PER_BLOCK && $self->line_marks[line];
  }
}

%extend mtkx_los_space {
  mtkx_los_space(const char *name, mtkx_address start, mtkx_address end, size_t threshold) {
    mtkx_los_space *s = (mtkx_los_space *)calloc(1, sizeof *s);
    if (s != NULL && mtkx_los_space_init(s, name, start, end, threshold) != MTKX_OK) {
      free(s);
      s = NULL;
    }
    return s;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_los_space, mtkx_los_space_destroy)

%extend mtkx_slot_buffer {
  mtkx_slot_buffer() {
    mtkx_slot_buffer *b = (mtkx_slot_buffer *)calloc(1, sizeof *b);
    if (b != NULL) {
      mtkx_slot_buffer_init(b);
    }
    return b;
  }
  /** Record @p i (borrowed; NULL when out of range). */
  mtkx_slot *get(size_t i) { return i < $self->size ? &$self->data[i] : NULL; }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_slot_buffer, mtkx_slot_buffer_destroy)

%extend mtkx_card_table {
  mtkx_card_table(mtkx_address base, size_t region_bytes, size_t card_bytes) {
    mtkx_card_table *t = (mtkx_card_table *)calloc(1, sizeof *t);
    if (t != NULL && mtkx_card_table_init(t, base, region_bytes, card_bytes) != MTKX_OK) {
      free(t);
      t = NULL;
    }
    return t;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_card_table, mtkx_card_table_destroy)

%extend mtkx_satb_queue {
  mtkx_satb_queue() {
    mtkx_satb_queue *q = (mtkx_satb_queue *)calloc(1, sizeof *q);
    if (q != NULL) {
      mtkx_satb_queue_init(q);
    }
    return q;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_satb_queue, mtkx_satb_queue_destroy)

%extend mtkx_work_queue {
  mtkx_work_queue() {
    mtkx_work_queue *q = (mtkx_work_queue *)calloc(1, sizeof *q);
    if (q != NULL) {
      mtkx_work_queue_init(q);
    }
    return q;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_work_queue, mtkx_work_queue_destroy)

%extend mtkx_task_pipeline {
  mtkx_task_pipeline() {
    mtkx_task_pipeline *p = (mtkx_task_pipeline *)calloc(1, sizeof *p);
    if (p != NULL) {
      mtkx_task_pipeline_init(p);
    }
    return p;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_task_pipeline, mtkx_task_pipeline_destroy)

%extend mtkx_phase_state {
  mtkx_phase_state() {
    mtkx_phase_state *s = (mtkx_phase_state *)calloc(1, sizeof *s);
    if (s != NULL) {
      mtkx_phase_state_init(s);
    }
    return s;
  }
  ~mtkx_phase_state() { free($self); }
}

%extend mtkx_task_chain {
  mtkx_task_chain(mtkx_task_policy policy) {
    mtkx_task_chain *c = (mtkx_task_chain *)calloc(1, sizeof *c);
    if (c != NULL) {
      mtkx_task_chain_init(c, policy);
    }
    return c;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_task_chain, mtkx_task_chain_destroy)

%extend mtkx_task_state {
  mtkx_task_state() {
    mtkx_task_state *s = (mtkx_task_state *)calloc(1, sizeof *s);
    if (s != NULL) {
      mtkx_task_state_init(s);
    }
    return s;
  }
}
MTKX_SWIG_PLAIN_DTOR(mtkx_task_state, mtkx_task_state_destroy)

/* returned by value from mtkx_task_ok / mtkx_task_err */
MTKX_SWIG_PLAIN_DTOR(mtkx_task_result, mtkx_task_result_free)
MTKX_SWIG_PLAIN_DTOR(mtkx_memo, mtkx_memo_destroy)

/* ---- coordinator thread records own their name in bindings ---------- */
%extend mtkx_thread_record {
  mtkx_thread_record(mtkx_thread_id id, const char *name) {
    mtkx_thread_record *r = (mtkx_thread_record *)calloc(1, sizeof *r);
    if (r != NULL) {
      mtkx_thread_record_init(r, id, mtkx_strdup(name));
    }
    return r;
  }
  ~mtkx_thread_record() {
    mtkx_free((char *)$self->name);
    free($self);
  }
  /** Reports the addresses in @p roots (which must outlive the record's
   *  registration) as this mutator's global roots. */
  void set_root_vector(mtkx_address_vec *roots) {
    $self->root_provider = mtkx_vec_root_provider;
    $self->root_provider_ctx = roots;
  }
}

/* ---- parse inputs and results ---------------------------------------- */
%extend mtkx_parse_input {
  /** An input over a private copy of @p source. */
  mtkx_parse_input(const char *source, size_t length) {
    mtkx_parse_input *in = (mtkx_parse_input *)calloc(1, sizeof *in);
    char *copy = mtkx_strndup(source != NULL ? source : "", source != NULL ? length : 0);
    if (in == NULL || copy == NULL) {
      free(in);
      mtkx_free(copy);
      return NULL;
    }
    in->source = copy;
    in->length = source != NULL ? length : 0;
    return in;
  }
  ~mtkx_parse_input() {
    mtkx_free((char *)$self->source);
    free($self);
  }
  /** The (copied) source text. */
  const char *text() const { return $self->source; }
}

%extend mtkx_parse_error {
  /** Expected label @p i (NULL when out of range). */
  const char *expected_at(size_t i) const {
    return i < $self->expected_count ? $self->expected[i] : NULL;
  }
}

%extend mtkx_parse_result {
  ~mtkx_parse_result() {
    mtkx_swig_release_text_($self->value);
    mtkx_parse_result_free($self);
    free($self);
  }
}

%extend mtkx_parse_value {
  char char_value() const { return $self->kind == MTKX_PV_CHAR ? $self->as.ch : '\0'; }
  bool bool_value() const { return $self->kind == MTKX_PV_BOOL && $self->as.boolean; }
  /** Pair halves (borrowed). */
  mtkx_parse_value *first() const {
    return $self->kind == MTKX_PV_PAIR ? $self->as.pair.first : NULL;
  }
  mtkx_parse_value *second() const {
    return $self->kind == MTKX_PV_PAIR ? $self->as.pair.second : NULL;
  }
  /** List length / list item @p i (borrowed). */
  size_t count() const { return $self->kind == MTKX_PV_LIST ? $self->as.list.count : 0; }
  mtkx_parse_value *item(size_t i) const {
    return ($self->kind == MTKX_PV_LIST && i < $self->as.list.count) ? $self->as.list.items[i]
                                                                      : NULL;
  }
  /** Optional contents (borrowed; NULL when empty). */
  mtkx_parse_value *inner() const {
    return $self->kind == MTKX_PV_OPTIONAL ? $self->as.optional : NULL;
  }
  /** Match span (borrowed). */
  mtkx_peg_match *span() {
    return $self->kind == MTKX_PV_MATCH ? &$self->as.match : NULL;
  }
}

/* ---- handles ---------------------------------------------------------- */
%extend mtkx_mark_sweep_plan {
  mtkx_mark_sweep_plan(size_t heap_bytes) { return mtkx_mark_sweep_plan_create(heap_bytes); }
  ~mtkx_mark_sweep_plan() { mtkx_mark_sweep_plan_destroy($self); }
}
%extend mtkx_semi_space_plan {
  mtkx_semi_space_plan(size_t semi_space_bytes) {
    return mtkx_semi_space_plan_create(semi_space_bytes);
  }
  ~mtkx_semi_space_plan() { mtkx_semi_space_plan_destroy($self); }
}
%extend mtkx_immix_plan {
  mtkx_immix_plan(size_t heap_bytes) { return mtkx_immix_plan_create(heap_bytes); }
  ~mtkx_immix_plan() { mtkx_immix_plan_destroy($self); }
}
%extend mtkx_generational_plan {
  mtkx_generational_plan(size_t nursery_bytes, size_t mature_bytes) {
    return mtkx_generational_plan_create(nursery_bytes, mature_bytes);
  }
  ~mtkx_generational_plan() { mtkx_generational_plan_destroy($self); }
}
%extend mtkx_coordinator {
  mtkx_coordinator(size_t expected_mutators) {
    return mtkx_coordinator_create(expected_mutators);
  }
  ~mtkx_coordinator() { mtkx_coordinator_destroy($self); }
}
%extend mtkx_ast_node {
  ~mtkx_ast_node() { mtkx_ast_free($self); }
}
%extend mtkx_parser {
  ~mtkx_parser() { mtkx_parser_release($self); }
}
%extend mtkx_peg_definition {
  mtkx_peg_definition() { return mtkx_peg_definition_create(); }
  ~mtkx_peg_definition() { mtkx_peg_definition_destroy($self); }
}
%extend mtkx_peg_matcher {
  ~mtkx_peg_matcher() {
    mtkx_swig_release_text_($self);
    mtkx_peg_matcher_destroy($self);
  }
  /** Feeds an arm matching any visible rule on @p channel. */
  mtkx_status feed_channel(const char *channel, mtkx_peg_action_fn fn, void *ctx) {
    return mtkx_peg_matcher_feed($self, mtkx_peg_channel_arm(channel, fn, ctx));
  }
}

/* ==========================================================================
 *  Binding helpers (safe replacements and scripting conveniences)
 * ========================================================================== */

%mutable;
%inline %{

/* ---------------- raw memory for scripts ------------------------------ */

/** Allocates @p bytes of zeroed, MTKX_MAX_ALIGN-aligned storage (0 on failure). */
mtkx_address mtkx_buffer_alloc(size_t bytes) {
  return mtkx_as_address(calloc(bytes != 0 ? bytes : 1, 1));
}
/** Releases storage from mtkx_buffer_alloc. */
void mtkx_buffer_free(mtkx_address address) { free(mtkx_from_address(void, address)); }

/** Reads the word at address + offset. */
mtkx_address mtkx_peek_word(mtkx_address address, size_t offset) {
  mtkx_address value;
  memcpy(&value, mtkx_from_address(char, address) + offset, sizeof value);
  return value;
}
/** Writes the word at address + offset. */
void mtkx_poke_word(mtkx_address address, size_t offset, mtkx_address value) {
  memcpy(mtkx_from_address(char, address) + offset, &value, sizeof value);
}
/** Reads the byte at address + offset. */
unsigned char mtkx_peek_byte(mtkx_address address, size_t offset) {
  return *(mtkx_from_address(unsigned char, address) + offset);
}
/** Writes the byte at address + offset. */
void mtkx_poke_byte(mtkx_address address, size_t offset, unsigned char value) {
  *(mtkx_from_address(unsigned char, address) + offset) = value;
}
/** Fills @p bytes bytes at @p address with @p value. */
void mtkx_memory_fill(mtkx_address address, unsigned char value, size_t bytes) {
  memset(mtkx_from_address(void, address), value, bytes);
}

/* ---------------- units, constants, small queries --------------------- */

size_t mtkx_b(unsigned long long n) { return MTKX_B(n); }
size_t mtkx_kb(unsigned long long n) { return MTKX_KB(n); }
size_t mtkx_mb(unsigned long long n) { return MTKX_MB(n); }
size_t mtkx_gb(unsigned long long n) { return MTKX_GB(n); }

/** Byte count of a literal such as "1.5_MB"; *status_out receives the mtkx_status. */
double mtkx_swig_units_parse_literal(const char *input, int *status_out) {
  long double value = 0.0L;
  *status_out = (int)mtkx_units_parse_literal(input, &value);
  return (double)value;
}

/** Size-class threshold @p i of the segregated allocator (0 when out of range). */
size_t mtkx_segregated_size_class(size_t i) {
  return i < MTKX_SEGREGATED_CLASS_COUNT ? mtkx_segregated_size_classes[i] : 0;
}

/** Reads a whole file; NULL on failure. */
char *mtkx_load_file(const char *path) {
  char *text = NULL;
  size_t length = 0;
  if (mtkx_load_parse_input_file(path, &text, &length, NULL) != MTKX_OK) {
    return NULL;
  }
  return text;
}

/* ---------------- built-in callbacks ---------------------------------- */

/** Up to 16 reference-field offsets describing an object layout. */
typedef struct mtkx_field_layout {
  size_t count;
  size_t offsets[16];
} mtkx_field_layout;

/** Trace: reports every nonzero word at the layout's field offsets. */
bool mtkx_field_trace(mtkx_address object, mtkx_address_vec *children, void *ctx) {
  const mtkx_field_layout *layout = (const mtkx_field_layout *)ctx;
  size_t i;
  for (i = 0; layout != NULL && i < layout->count; ++i) {
    const mtkx_address field = mtkx_peek_word(object, layout->offsets[i]);
    if (field != 0 && mtkx_address_vec_push(children, field) != MTKX_OK) {
      return false;
    }
  }
  return true;
}

/** Root provider: reports the addresses of the mtkx_address_vec in ctx. */
mtkx_status mtkx_vec_root_provider(mtkx_address_vec *roots, void *ctx) {
  const mtkx_address_vec *source = (const mtkx_address_vec *)ctx;
  return source == NULL ? MTKX_OK : mtkx_address_vec_append(roots, source->data, source->size);
}

/* ---------------- collection with address vectors --------------------- */

mtkx_plan_stats_result mtkx_plan_collect_vec(mtkx_plan *plan, const mtkx_address_vec *roots,
                                             mtkx_trace trace) {
  return mtkx_plan_collect(plan, roots != NULL ? roots->data : NULL,
                           roots != NULL ? roots->size : 0, trace);
}

/* ---------------- spaces ---------------------------------------------- */

/** Bump-allocates in an Immix block; 0 when the block is full. */
mtkx_address mtkx_swig_immix_block_allocate(mtkx_immix_block *block, size_t bytes,
                                            size_t alignment) {
  mtkx_address address = 0;
  return mtkx_immix_block_allocate(block, bytes, alignment, &address) ? address : 0;
}

/* ---------------- task pipelines and chains (names copied) ------------ */

mtkx_status mtkx_task_pipeline_append_phase(mtkx_task_pipeline *pipeline, const char *name,
                                            mtkx_phase_fn fn, void *ctx, bool stop_on_error) {
  return mtkx_task_pipeline_append(pipeline, mtkx_make_phase(name, fn, ctx, stop_on_error));
}

mtkx_status mtkx_task_chain_append_task(mtkx_task_chain *chain, const char *name,
                                        mtkx_task_fn fn, void *ctx) {
  return mtkx_task_chain_append(chain, mtkx_task_make(name, fn, ctx));
}

/* ---------------- AST ---------------------------------------------------- */

/** An inner node with no children yet (add them with mtkx_ast_append_child). */
mtkx_ast_node *mtkx_ast_inner(const char *tag) { return mtkx_ast_node_new(tag, 0, NULL); }

/** Appends a deep copy of @p child (the caller keeps @p child). */
mtkx_status mtkx_swig_ast_append_child(mtkx_ast_node *node, const mtkx_ast_node *child) {
  mtkx_ast_node *copy;
  if (node == NULL || child == NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  copy = mtkx_ast_clone(child);
  return copy != NULL ? mtkx_ast_append_child(node, copy) : MTKX_ERR_NOMEM;
}

/* ---------------- task state ------------------------------------------- */

/** Stores a copy of @p result under @p name (the caller keeps @p result). */
mtkx_status mtkx_swig_task_state_set(mtkx_task_state *state, const char *name,
                                     const mtkx_task_result *result) {
  if (result == NULL) {
    return MTKX_ERR_INVALID_ARGUMENT;
  }
  return mtkx_task_state_set(state, name, result->ok ? mtkx_task_ok(result->text)
                                                     : mtkx_task_err(result->text));
}

/* ---------------- PEG ---------------------------------------------------- */

/** Adds a rule; returns it (borrowed, owned by the definition) or NULL. */
mtkx_peg_rule *mtkx_swig_peg_definition_add_rule(mtkx_peg_definition *definition,
                                                 const char *pattern,
                                                 mtkx_peg_action_fn action, void *ctx) {
  mtkx_peg_rule *rule = NULL;
  (void)mtkx_peg_definition_add_rule(definition, pattern, action, ctx, &rule);
  return rule;
}

/** Visible rule @p index, parent rules first (NULL when out of range). */
const mtkx_peg_rule *mtkx_peg_definition_rule_at(const mtkx_peg_definition *definition,
                                                 size_t index) {
  const mtkx_peg_rule *rule = NULL;
  size_t total = mtkx_peg_definition_rules(definition, NULL, 0);
  const mtkx_peg_rule **all;
  if (index >= total) {
    return NULL;
  }
  all = (const mtkx_peg_rule **)malloc(total * sizeof *all);
  if (all != NULL) {
    mtkx_peg_definition_rules(definition, all, total);
    rule = all[index];
    free(all);
  }
  return rule;
}

/** End of the rule's match at @p pos, or -1 when it does not match. */
long long mtkx_swig_peg_rule_try_match(const mtkx_peg_rule *rule, const char *text,
                                       size_t length, size_t pos) {
  size_t end = 0;
  return mtkx_peg_rule_try_match(rule, text, length, pos, &end) ? (long long)end : -1;
}

/** A matcher over a private copy of @p text. */
mtkx_peg_matcher *mtkx_swig_peg_matcher_create(const mtkx_peg_definition *definition,
                                               const char *text, size_t length) {
  char *copy = mtkx_strndup(text != NULL ? text : "", text != NULL ? length : 0);
  mtkx_peg_matcher *m;
  if (copy == NULL) {
    return NULL;
  }
  m = mtkx_peg_matcher_create(definition, copy, text != NULL ? length : 0);
  if (m == NULL) {
    mtkx_free(copy);
    return NULL;
  }
  mtkx_swig_attach_text_(m, copy);
  return m;
}

void mtkx_swig_peg_matcher_destroy(mtkx_peg_matcher *matcher) {
  mtkx_swig_release_text_(matcher);
  mtkx_peg_matcher_destroy(matcher);
}

mtkx_parser *mtkx_peg_seq2(mtkx_parser *a, mtkx_parser *b) {
  return mtkx_peg_seq(2, mtkx_parser_retain(a), mtkx_parser_retain(b));
}
mtkx_parser *mtkx_peg_seq3(mtkx_parser *a, mtkx_parser *b, mtkx_parser *c) {
  return mtkx_peg_seq(3, mtkx_parser_retain(a), mtkx_parser_retain(b), mtkx_parser_retain(c));
}
mtkx_parser *mtkx_peg_choice2(mtkx_parser *a, mtkx_parser *b) {
  return mtkx_peg_choice(2, mtkx_parser_retain(a), mtkx_parser_retain(b));
}
mtkx_parser *mtkx_peg_choice3(mtkx_parser *a, mtkx_parser *b, mtkx_parser *c) {
  return mtkx_peg_choice(3, mtkx_parser_retain(a), mtkx_parser_retain(b),
                         mtkx_parser_retain(c));
}

/* ---------------- parser combinators that borrow their operands ------- */

mtkx_parser *mtkx_swig_parser_seq(mtkx_parser *a, mtkx_parser *b) {
  return mtkx_parser_seq(mtkx_parser_retain(a), mtkx_parser_retain(b));
}
mtkx_parser *mtkx_swig_parser_alt(mtkx_parser *a, mtkx_parser *b) {
  return mtkx_parser_alt(mtkx_parser_retain(a), mtkx_parser_retain(b));
}
mtkx_parser *mtkx_swig_parser_many(mtkx_parser *p) {
  return mtkx_parser_many(mtkx_parser_retain(p));
}
mtkx_parser *mtkx_swig_parser_optional(mtkx_parser *p) {
  return mtkx_parser_optional(mtkx_parser_retain(p));
}
mtkx_parser *mtkx_swig_parser_try(mtkx_parser *p) {
  return mtkx_parser_try(mtkx_parser_retain(p));
}
mtkx_parser *mtkx_swig_parser_labeled(mtkx_parser *p, const char *label) {
  return mtkx_parser_labeled(mtkx_parser_retain(p), label);
}
mtkx_parser *mtkx_swig_peg_opt(mtkx_parser *p) { return mtkx_peg_opt(mtkx_parser_retain(p)); }
mtkx_parser *mtkx_swig_peg_many(mtkx_parser *p) { return mtkx_peg_many(mtkx_parser_retain(p)); }
mtkx_parser *mtkx_swig_peg_many1(mtkx_parser *p) {
  return mtkx_peg_many1(mtkx_parser_retain(p));
}
mtkx_parser *mtkx_swig_peg_and(mtkx_parser *p) { return mtkx_peg_and(mtkx_parser_retain(p)); }
mtkx_parser *mtkx_swig_peg_not(mtkx_parser *p) { return mtkx_peg_not(mtkx_parser_retain(p)); }
mtkx_parser *mtkx_swig_peg_channel(const char *channel, mtkx_parser *p) {
  return mtkx_peg_channel(channel, mtkx_parser_retain(p));
}
mtkx_parser *mtkx_swig_peg_as_span(mtkx_parser *p) {
  return mtkx_peg_as_span(mtkx_parser_retain(p));
}

/** Parses the whole of @p source; match spans in the result stay valid
 *  for the result's lifetime (the binding keeps a copy of the text). */
mtkx_parse_result mtkx_swig_run_parser(const mtkx_parser *parser, const char *source,
                                       size_t length) {
  char *copy = mtkx_strndup(source != NULL ? source : "", source != NULL ? length : 0);
  mtkx_parse_result r;
  if (copy == NULL) {
    return mtkx_parse_fail(0, MTKX_PARSE_COMMITTED, "<out of memory>");
  }
  r = mtkx_run_parser(parser, copy, source != NULL ? length : 0);
  if (r.ok && r.value != NULL) {
    mtkx_swig_attach_text_(r.value, copy);
  } else {
    mtkx_free(copy);
  }
  return r;
}

%}
%immutable;

/* Built-in C callbacks, usable wherever a callback is expected. */
%constant mtkx_trace_fn MTKX_FIELD_TRACE = mtkx_field_trace;
%constant mtkx_root_provider_fn MTKX_VEC_ROOT_PROVIDER = mtkx_vec_root_provider;

%extend mtkx_field_layout {
  mtkx_field_layout() { return (mtkx_field_layout *)calloc(1, sizeof(mtkx_field_layout)); }
  ~mtkx_field_layout() { free($self); }
  /** Declares a reference field at @p offset; false when the layout is full. */
  bool add_field(size_t offset) {
    if ($self->count >= 16) {
      return false;
    }
    $self->offsets[$self->count++] = offset;
    return true;
  }
  /** A trace scanning this layout (the layout must outlive collections). */
  mtkx_trace trace() { return mtkx_trace_make(mtkx_field_trace, $self); }
}

/* Setters for the fields scripts are meant to change. */
%mutable;
%extend mtkx_peg_rule {
  void set_must_fail(bool value) { $self->must_fail = value; }
  void set_fire_on_neg(bool value) { $self->fire_on_neg = value; }
  void set_if_succeed(mtkx_peg_rule *rule) { $self->if_succeed = rule; }
}
%extend mtkx_rewrite_set {
  void set_max_iterations(size_t value) { $self->max_iterations = value; }
}
%extend mtkx_task_chain {
  void set_policy(mtkx_task_policy policy) { $self->policy = policy; }
}