/**
 * @file memtkx/dsl/parser.h
 * @brief Parser combinators (C++ `Parser<T>`, `ch`, `satisfy`, `&`, `|`,
 *        unary `*`, `optional`, `try_parse`, `labeled`, `run_parser`).
 *
 * C has no `Parser<T>`, so parsers produce dynamically typed values
 * (::mtkx_parse_value): a character, a boolean, a pair, a list, an
 * optional, a PEG match span or a user pointer. The value shapes follow
 * the C++ types exactly:
 *
 *   | Combinator               | C++ result            | C value kind       |
 *   |--------------------------|-----------------------|--------------------|
 *   | mtkx_parser_ch/satisfy   | char                  | MTKX_PV_CHAR       |
 *   | mtkx_parser_seq(a, b)    | std::pair<A, B>       | MTKX_PV_PAIR       |
 *   | mtkx_parser_alt(a, b)    | T                     | value of a or b    |
 *   | mtkx_parser_many(p)      | std::vector<T>        | MTKX_PV_LIST       |
 *   | mtkx_parser_optional(p)  | std::optional<T>      | MTKX_PV_OPTIONAL   |
 *   | mtkx_peg_*  (peg.h)      | PEGMatch / bool       | MTKX_PV_MATCH/BOOL |
 *
 * Ownership: parsers are reference counted. Every combinator *consumes*
 * the references passed to it, so expressions nest naturally:
 *
 * @code
 *   mtkx_parser *ab = mtkx_parser_seq(mtkx_parser_ch('a'), mtkx_parser_ch('b'));
 *   mtkx_parse_result r = mtkx_run_parser(ab, "ab", 2);
 *   // r.ok, r.value->kind == MTKX_PV_PAIR, first->as.ch == 'a'
 *   mtkx_parse_result_free(&r);
 *   mtkx_parser_release(ab);
 * @endcode
 *
 * To use one parser in two places, take an extra reference with
 * mtkx_parser_retain(). A combinator given a NULL operand (e.g. from an
 * earlier allocation failure) releases its other operands and returns
 * NULL, so a whole expression needs one NULL check.
 */
#ifndef MEMTKX_DSL_PARSER_H
#define MEMTKX_DSL_PARSER_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/core/vec.h"

MTKX_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Input                                                             */
/* ------------------------------------------------------------------ */

/** Source text and cursor (C++ `ParsecInput`). The text is borrowed. */
typedef struct mtkx_parse_input {
  const char *source;
  size_t length;
  size_t pos;
} mtkx_parse_input;

/** Builds an input over @p length bytes of @p source, positioned at 0. */
MTKX_INLINE mtkx_parse_input mtkx_parse_input_make(const char *source, size_t length) {
  mtkx_parse_input in;
  in.source = source;
  in.length = length;
  in.pos = 0;
  return in;
}

/** @return the current character, or '\0' at end of input. */
MTKX_INLINE char mtkx_parse_input_peek(const mtkx_parse_input *in) {
  return in->pos < in->length ? in->source[in->pos] : '\0';
}

/** @return true at end of input. */
MTKX_INLINE bool mtkx_parse_input_eof(const mtkx_parse_input *in) {
  return in->pos >= in->length;
}

/** Consumes and returns one character ('\0' at end of input). */
MTKX_INLINE char mtkx_parse_input_consume(mtkx_parse_input *in) {
  return mtkx_parse_input_eof(in) ? '\0' : in->source[in->pos++];
}

/** @return the text between @p start and the cursor. */
MTKX_INLINE mtkx_strview mtkx_parse_input_span(const mtkx_parse_input *in, size_t start) {
  if (start > in->pos) {
    start = in->pos;
  }
  return mtkx_strview_make(in->source + start, in->pos - start);
}

/* ------------------------------------------------------------------ */
/*  Errors                                                            */
/* ------------------------------------------------------------------ */

/** Whether alternatives may still be tried after a failure. */
typedef enum mtkx_parse_failure_kind {
  MTKX_PARSE_SOFT = 0,      /**< nothing consumed; alternatives may run */
  MTKX_PARSE_COMMITTED = 1  /**< input consumed; alternatives are skipped */
} mtkx_parse_failure_kind;

/** Failure description (C++ `ParseError`). Owns its label strings. */
typedef struct mtkx_parse_error {
  size_t pos;
  mtkx_parse_failure_kind kind;
  char **expected;       /**< labels of what was expected at @ref pos */
  size_t expected_count;
  bool out_of_memory;    /**< a native allocation failed during parsing */
} mtkx_parse_error;

/** Releases the labels and resets @p error to an empty soft error at 0. */
MTKX_API void mtkx_parse_error_clear(mtkx_parse_error *error);

/** Appends a copy of @p label. */
MTKX_API mtkx_status mtkx_parse_error_add_expected(mtkx_parse_error *error,
                                                   const char *label);

/** Replaces all labels with a copy of @p label. */
MTKX_API mtkx_status mtkx_parse_error_set_expected(mtkx_parse_error *error,
                                                   const char *label);

/**
 * @brief Merges @p src into @p dst (C++ `merge_error`): the furthest
 *        position wins; at equal positions labels are united and
 *        committed-ness is sticky.
 */
MTKX_API void mtkx_parse_error_merge(mtkx_parse_error *dst, const mtkx_parse_error *src);

/** Deep-copies @p src into @p dst (dst is cleared first). */
MTKX_API mtkx_status mtkx_parse_error_copy(mtkx_parse_error *dst,
                                           const mtkx_parse_error *src);

/** @return true when @p label is among the expected labels. */
MTKX_API bool mtkx_parse_error_expects(const mtkx_parse_error *error, const char *label);

/* ------------------------------------------------------------------ */
/*  Values                                                            */
/* ------------------------------------------------------------------ */

struct mtkx_peg_rule;

/** A matched span (C++ `PEGMatch`). Views point into the parsed source. */
typedef struct mtkx_peg_match {
  mtkx_strview value;          /**< matched text */
  size_t begin;                /**< start offset */
  size_t end;                  /**< one-past-end offset */
  struct mtkx_peg_rule *rule;  /**< rule that produced the match, or NULL */
  const char *channel;         /**< channel name, e.g. "@DEFAULT" (borrowed) */
  void *user_data;             /**< free for the application */
  size_t line;                 /**< 1-based line of begin */
  size_t column;               /**< 1-based column of begin */
  bool ignored;                /**< routed to the @IGNORE channel */
} mtkx_peg_match;

/** @return end - begin. */
MTKX_INLINE size_t mtkx_peg_match_length(const mtkx_peg_match *match) {
  return match->end - match->begin;
}

/** Kinds of parse values. */
typedef enum mtkx_parse_value_kind {
  MTKX_PV_CHAR = 0,
  MTKX_PV_BOOL = 1,
  MTKX_PV_PAIR = 2,
  MTKX_PV_LIST = 3,
  MTKX_PV_OPTIONAL = 4,
  MTKX_PV_MATCH = 5,
  MTKX_PV_USER = 6
} mtkx_parse_value_kind;

/** A dynamically typed parse value; owns its sub-values. */
typedef struct mtkx_parse_value mtkx_parse_value;
struct mtkx_parse_value {
  mtkx_parse_value_kind kind;
  union {
    char ch;
    bool boolean;
    struct {
      mtkx_parse_value *first;
      mtkx_parse_value *second;
    } pair;
    struct {
      mtkx_parse_value **items;
      size_t count;
      size_t capacity;
    } list;
    mtkx_parse_value *optional; /**< NULL when empty */
    mtkx_peg_match match;
    struct {
      void *ptr;
      void (*destroy)(void *ptr);
    } user;
  } as;
};

/** New character value (NULL on OOM). */
MTKX_API mtkx_parse_value *mtkx_parse_value_char(char ch);
/** New boolean value (NULL on OOM). */
MTKX_API mtkx_parse_value *mtkx_parse_value_bool(bool value);
/** New pair owning both halves; a NULL half frees the other and yields NULL. */
MTKX_API mtkx_parse_value *mtkx_parse_value_pair(mtkx_parse_value *first,
                                                 mtkx_parse_value *second);
/** New empty list (NULL on OOM). */
MTKX_API mtkx_parse_value *mtkx_parse_value_list(void);
/** Appends @p item (ownership transfers; freed on failure). */
MTKX_API mtkx_status mtkx_parse_value_list_push(mtkx_parse_value *list,
                                                mtkx_parse_value *item);
/** New empty optional (NULL on OOM). */
MTKX_API mtkx_parse_value *mtkx_parse_value_none(void);
/** New optional holding @p inner; NULL inner yields NULL. */
MTKX_API mtkx_parse_value *mtkx_parse_value_some(mtkx_parse_value *inner);
/** New match value (NULL on OOM). */
MTKX_API mtkx_parse_value *mtkx_parse_value_match(const mtkx_peg_match *match);
/**
 * New user value; @p destroy (may be NULL) runs when the value is freed.
 * On OOM, destroy(ptr) is called and NULL returned.
 */
MTKX_API mtkx_parse_value *mtkx_parse_value_user(void *ptr, void (*destroy)(void *));
/** Frees a value tree (NULL is ignored). */
MTKX_API void mtkx_parse_value_free(mtkx_parse_value *value);

/* ------------------------------------------------------------------ */
/*  Results                                                           */
/* ------------------------------------------------------------------ */

/** Value or error (C++ `ExpectedResult<T>` / `ParseOutcome<T>`). */
typedef struct mtkx_parse_result {
  bool ok;
  mtkx_parse_value *value; /**< owned; non-NULL when ok */
  mtkx_parse_error error;  /**< meaningful when !ok */
} mtkx_parse_result;

/** Success holding @p value; a NULL value becomes an out-of-memory failure. */
MTKX_API mtkx_parse_result mtkx_parse_ok(mtkx_parse_value *value);

/** Failure at @p pos with an optional expected label (C++ `failure`). */
MTKX_API mtkx_parse_result mtkx_parse_fail(size_t pos, mtkx_parse_failure_kind kind,
                                           const char *label);

/** Failure at the input cursor expecting @p label (C++ `fail_expected`). */
MTKX_API mtkx_parse_result mtkx_parse_fail_expected(const mtkx_parse_input *in,
                                                    const char *label,
                                                    mtkx_parse_failure_kind kind);

/** Failure carrying (moving) an existing error. */
MTKX_API mtkx_parse_result mtkx_parse_fail_with(mtkx_parse_error *moved_error);

/** Moves the value out of @p result (the caller owns it afterwards). */
MTKX_API mtkx_parse_value *mtkx_parse_result_take_value(mtkx_parse_result *result);

/** Frees the value and error labels held by @p result. */
MTKX_API void mtkx_parse_result_free(mtkx_parse_result *result);

/* ------------------------------------------------------------------ */
/*  Parsers                                                           */
/* ------------------------------------------------------------------ */

/** A reference-counted parser (opaque). */
typedef struct mtkx_parser mtkx_parser;

/**
 * @brief Parsing function. Must return a result whose value (on success)
 *        is a fresh, owned value; on failure the input position is
 *        restored by the combinators that need it.
 */
typedef mtkx_parse_result (*mtkx_parse_fn)(mtkx_parse_input *in, void *ctx);

/**
 * @brief Wraps a function as a parser (C++ `dsl::parser(fn)`).
 * @param ctx_free called with @p ctx when the last reference is released
 *        (may be NULL). On allocation failure it is called immediately.
 * @return a parser with one reference, or NULL on OOM.
 */
MTKX_API mtkx_parser *mtkx_parser_new(mtkx_parse_fn fn, void *ctx,
                                      void (*ctx_free)(void *ctx));

/** Adds a reference and returns @p parser. */
MTKX_API mtkx_parser *mtkx_parser_retain(mtkx_parser *parser);

/** Drops a reference; the parser is destroyed with its last reference. */
MTKX_API void mtkx_parser_release(mtkx_parser *parser);

/** Runs @p parser once at the input cursor (C++ `p(in)`). */
MTKX_API mtkx_parse_result mtkx_parser_parse(const mtkx_parser *parser,
                                             mtkx_parse_input *in);

/** Matches exactly @p c; expected label is the character (C++ `ch`). */
MTKX_API mtkx_parser *mtkx_parser_ch(char c);

/** Character predicate for mtkx_parser_satisfy(). */
typedef bool (*mtkx_char_pred_fn)(char c, void *ctx);

/** Matches one character accepted by @p pred (C++ `satisfy`). */
MTKX_API mtkx_parser *mtkx_parser_satisfy(mtkx_char_pred_fn pred, void *ctx,
                                          const char *label);

/** Sequence a then b; pair value; committed when b fails after a consumed (C++ `a & b`). */
MTKX_API mtkx_parser *mtkx_parser_seq(mtkx_parser *a, mtkx_parser *b);

/** Ordered choice; b runs only after a soft failure of a (C++ `a | b`). */
MTKX_API mtkx_parser *mtkx_parser_alt(mtkx_parser *a, mtkx_parser *b);

/** Zero or more repetitions into a list (C++ unary `*p`). */
MTKX_API mtkx_parser *mtkx_parser_many(mtkx_parser *p);

/** Optional value; committed failures still propagate (C++ `optional`). */
MTKX_API mtkx_parser *mtkx_parser_optional(mtkx_parser *p);

/** Restores the position on failure and makes it soft (C++ `try_parse`). */
MTKX_API mtkx_parser *mtkx_parser_try(mtkx_parser *p);

/** Replaces the expected labels of a failure with @p label (C++ `labeled`). */
MTKX_API mtkx_parser *mtkx_parser_labeled(mtkx_parser *p, const char *label);

/**
 * @brief Runs @p parser over the whole of @p source (C++ `run_parser`).
 *
 * Unconsumed trailing input is a committed failure at the stop position
 * expecting "<end-of-input>".
 */
MTKX_API mtkx_parse_result mtkx_run_parser(const mtkx_parser *parser, const char *source,
                                           size_t length);

/* ------------------------------------------------------------------ */
/*  Semantic actions and productions                                  */
/* ------------------------------------------------------------------ */

/** Span information passed to semantic actions (C++ `ParserStage`). */
typedef struct mtkx_parser_stage {
  size_t begin;
  size_t end;
  mtkx_strview span;
} mtkx_parser_stage;

/** A key/value pair of the semantics table. */
typedef struct mtkx_semantic_entry {
  const char *key;
  const char *value;
} mtkx_semantic_entry;

/** Semantic action invoked after a successful parse. */
typedef void (*mtkx_parse_action_fn)(const mtkx_semantic_entry *semantics, size_t count,
                                     const mtkx_parser_stage *stage, void *out, void *ctx);

/**
 * @brief Runs @p parser and, on success, calls @p action with the matched
 *        span (C++ `parse_with_action`). The parse value is discarded.
 * @return true when the parse succeeded (and the action ran).
 */
MTKX_API bool mtkx_parse_with_action(const mtkx_parser *parser, mtkx_parse_input *in,
                                     const mtkx_semantic_entry *semantics, size_t count,
                                     mtkx_parse_action_fn action, void *ctx, void *out);

/** A production handler keyed by id (C++ `production<"id">(fn)`). */
typedef struct mtkx_production {
  const char *id;
  mtkx_parse_action_fn fn;
  void *ctx;
} mtkx_production;

/** Builds a production entry. */
MTKX_INLINE mtkx_production mtkx_production_make(const char *id, mtkx_parse_action_fn fn,
                                                 void *ctx) {
  mtkx_production p;
  p.id = id;
  p.fn = fn;
  p.ctx = ctx;
  return p;
}

/** @return the first production whose id equals @p id, or NULL. */
MTKX_API const mtkx_production *mtkx_production_find(const mtkx_production *productions,
                                                     size_t count, const char *id);

/**
 * @brief Reads a whole file as parser input (C++ `load_parse_input_file`).
 *
 * On success *out_text receives a NUL-terminated buffer (release with
 * mtkx_free) and *out_length its length. On failure, if @p out_message is
 * not NULL it receives a description (release with mtkx_free).
 *
 * @return MTKX_OK, MTKX_ERR_IO or MTKX_ERR_NOMEM.
 */
MTKX_API mtkx_status mtkx_load_parse_input_file(const char *path, char **out_text,
                                                size_t *out_length, char **out_message);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_PARSER_H */
