/**
 * @file memtkx/dsl/peg.h
 * @brief PEG grammars: rules, channels, tokenizing parse, a fluent matcher
 *        and PEG combinators over mtkx_parser (C++ `PEGDefinition`,
 *        `PEGRule`, `PEGMatcher`, `peg_seq`, `peg_choice`, ...).
 *
 * Patterns use the regex subset: literal characters, `.`, `[abc]`,
 * `[a-z]`, `[^...]`, and the quantifiers `*`, `+`, `?` and `!` (must-fail
 * lookahead) after an atom. `^`/`$` are accepted and ignored. Matching is
 * a greedy, non-backtracking prefix match at the current position.
 *
 * @code
 *   static void on_ident(mtkx_peg_match *m, void *ctx) { ++*(int *)ctx; }
 *
 *   int idents = 0;
 *   mtkx_peg_definition *peg = mtkx_peg_definition_create();
 *   mtkx_peg_rule *ident = NULL, *space = NULL;
 *   mtkx_peg_definition_add_rule(peg, "[a-z]+", on_ident, &idents, &ident);
 *   mtkx_peg_definition_add_rule(peg, "[ ]+", NULL, NULL, &space);
 *   mtkx_peg_rule_set_channel(space, MTKX_PEG_IGNORE_CHANNEL);
 *
 *   mtkx_peg_parse_result r = mtkx_peg_definition_parse(peg, "ab cd", 5);
 *   // r.success, idents == 2
 *   mtkx_peg_definition_destroy(peg);
 * @endcode
 */
#ifndef MEMTKX_DSL_PEG_H
#define MEMTKX_DSL_PEG_H

#include <stddef.h>

#include "memtkx/core/base.h"
#include "memtkx/dsl/parser.h"

MTKX_BEGIN_DECLS

/** Built-in channel for ignorable input (whitespace, comments ...). */
#define MTKX_PEG_IGNORE_CHANNEL "@IGNORE"
/** Built-in channel for ordinary input. */
#define MTKX_PEG_DEFAULT_CHANNEL "@DEFAULT"

/**
 * @brief Validates a channel name: non-empty and starting with '@'
 *        (C++ `new_peg_channel`, which threw).
 * @return MTKX_OK or MTKX_ERR_INVALID_ARGUMENT.
 */
MTKX_API mtkx_status mtkx_peg_channel_validate(const char *name);

/** @return true for the @IGNORE channel. */
MTKX_API bool mtkx_peg_channel_is_ignore(const char *name);

/** Semantic action invoked with a match. */
typedef void (*mtkx_peg_action_fn)(mtkx_peg_match *match, void *ctx);

/** An action and its context. */
typedef struct mtkx_peg_action {
  mtkx_peg_action_fn fn;
  void *ctx;
} mtkx_peg_action;

/** @internal Compiled pattern (opaque). */
typedef struct mtkx_peg_pattern mtkx_peg_pattern;

/**
 * @brief A PEG rule, owned by its definition. Fields after `compiled`
 *        are mutable at any time (C++ semantics); use the setters for the
 *        channel name and action.
 */
typedef struct mtkx_peg_rule {
  char *pattern_text;              /**< source pattern (read-only) */
  mtkx_peg_pattern *compiled;      /**< private */
  mtkx_peg_action semantic_action; /**< run when the rule matches during parse */
  char *channel;                   /**< channel name (use the setter) */
  bool must_fail;                  /**< succeed (consuming 1 char) only when the pattern fails */
  bool fire_on_neg;                /**< run the action on a must_fail success too */
  struct mtkx_peg_rule *if_succeed;/**< also attempted at the same position on success */
  size_t order;                    /**< declaration order within its definition */
  void *user_data;                 /**< free for the application (bindings, wrappers) */
} mtkx_peg_rule;

/** Sets the rule's channel to a copy of @p name (validated). */
MTKX_API mtkx_status mtkx_peg_rule_set_channel(mtkx_peg_rule *rule, const char *name);

/** Sets the rule's semantic action. */
MTKX_API void mtkx_peg_rule_set_action(mtkx_peg_rule *rule, mtkx_peg_action_fn fn,
                                       void *ctx);

/**
 * @brief Attempts the rule at @p pos (C++ `try_match`).
 * @return true and the match end in *end on success. A must_fail rule
 *         succeeds by consuming one character when its pattern fails.
 */
MTKX_API bool mtkx_peg_rule_try_match(const mtkx_peg_rule *rule, const char *text,
                                      size_t length, size_t pos, size_t *end);

/**
 * @brief Tests @p pattern for compilation errors without creating a rule.
 * @return NULL when valid, otherwise a static description
 *         (e.g. "unterminated character class").
 */
MTKX_API const char *mtkx_peg_pattern_error(const char *pattern);

/* ------------------------------------------------------------------ */
/*  Definitions                                                       */
/* ------------------------------------------------------------------ */

/** An ordered rule set with optional parent (opaque). */
typedef struct mtkx_peg_definition mtkx_peg_definition;

/** Creates a root definition (C++ `create_peg_definition`). NULL on OOM. */
MTKX_API mtkx_peg_definition *mtkx_peg_definition_create(void);

/**
 * @brief Creates a definition that sees @p parent's rules first
 *        (C++ `derive_peg_definition`). The parent must outlive it.
 */
MTKX_API mtkx_peg_definition *mtkx_peg_definition_derive(const mtkx_peg_definition *parent);

/** Destroys a definition and its rules (not its parent). */
MTKX_API void mtkx_peg_definition_destroy(mtkx_peg_definition *definition);

/** @return the parent definition or NULL. */
MTKX_API const mtkx_peg_definition *mtkx_peg_definition_parent(
    const mtkx_peg_definition *definition);

/**
 * @brief Adds a rule (C++ `add_rule<"pattern">(action)`).
 *
 * The returned rule pointer stays valid until the definition is destroyed.
 * @param action may be NULL.
 * @param out_rule receives the rule (may be NULL).
 * @return MTKX_OK, MTKX_ERR_INVALID_ARGUMENT for a bad pattern, or
 *         MTKX_ERR_NOMEM.
 */
MTKX_API mtkx_status mtkx_peg_definition_add_rule(mtkx_peg_definition *definition,
                                                  const char *pattern,
                                                  mtkx_peg_action_fn action, void *ctx,
                                                  mtkx_peg_rule **out_rule);

/**
 * @brief Lists the visible rules, parent rules first (C++ `rules()`).
 * @return the total number of visible rules; at most @p capacity are
 *         written to @p out (which may be NULL to query the count).
 */
MTKX_API size_t mtkx_peg_definition_rules(const mtkx_peg_definition *definition,
                                          const mtkx_peg_rule **out, size_t capacity);

/** Diagnostics of mtkx_peg_definition_parse (C++ `PEGParseResult`). */
typedef struct mtkx_peg_parse_result {
  bool success;
  size_t offset;  /**< failure offset, or input length on success */
  size_t line;    /**< 1-based */
  size_t column;  /**< 1-based */
  char message[96];/**< empty on success */
} mtkx_peg_parse_result;

/**
 * @brief Tokenizes @p text with ordered choice over all visible rules,
 *        running semantic actions as matches occur. Succeeds when the
 *        whole input is consumed. A rule matching empty input fails the
 *        parse rather than looping.
 */
MTKX_API mtkx_peg_parse_result mtkx_peg_definition_parse(
    const mtkx_peg_definition *definition, const char *text, size_t length);

/* ------------------------------------------------------------------ */
/*  Matcher                                                           */
/* ------------------------------------------------------------------ */

/** A match arm (C++ `rule(cb)`, `matcher.wildcard(cb)`). */
typedef struct mtkx_peg_arm {
  const mtkx_peg_rule *rule; /**< rule arm (NULL for wildcard/channel arms) */
  mtkx_peg_action action;    /**< overrides the rule's own action when set */
  bool is_wildcard;          /**< fires when no rule arm matched; consumes 1 char */
  bool is_channel;           /**< matches any visible rule on `channel` */
  const char *channel;       /**< channel for channel arms (copied on feed) */
} mtkx_peg_arm;

/** An arm binding @p rule to an action (NULL fn uses the rule's action). */
MTKX_API mtkx_peg_arm mtkx_peg_rule_arm(const mtkx_peg_rule *rule, mtkx_peg_action_fn fn,
                                        void *ctx);

/** A wildcard arm. */
MTKX_API mtkx_peg_arm mtkx_peg_wildcard_arm(mtkx_peg_action_fn fn, void *ctx);

/** An arm that matches any visible rule on @p channel. */
MTKX_API mtkx_peg_arm mtkx_peg_channel_arm(const char *channel, mtkx_peg_action_fn fn,
                                           void *ctx);

/** A streaming matcher over borrowed text (opaque). */
typedef struct mtkx_peg_matcher mtkx_peg_matcher;

/** Creates a matcher (C++ `peg_new_matcher`). NULL on OOM. */
MTKX_API mtkx_peg_matcher *mtkx_peg_matcher_create(const mtkx_peg_definition *definition,
                                                   const char *text, size_t length);

/** Destroys the matcher (C++ `close()` plus release). */
MTKX_API void mtkx_peg_matcher_destroy(mtkx_peg_matcher *matcher);

/** @return true when the whole text has been consumed. */
MTKX_API bool mtkx_peg_matcher_is_spent(const mtkx_peg_matcher *matcher);

/** @return the current offset. */
MTKX_API size_t mtkx_peg_matcher_position(const mtkx_peg_matcher *matcher);

/** Restricts rule arms to a single channel. */
MTKX_API mtkx_status mtkx_peg_matcher_only(mtkx_peg_matcher *matcher, const char *channel);

/** Adds a channel to the accepted set. */
MTKX_API mtkx_status mtkx_peg_matcher_include(mtkx_peg_matcher *matcher,
                                              const char *channel);

/** Rejects rule arms on @p channel. */
MTKX_API mtkx_status mtkx_peg_matcher_exclude(mtkx_peg_matcher *matcher,
                                              const char *channel);

/** @return whether rule arms on @p channel are currently allowed. */
MTKX_API bool mtkx_peg_matcher_channel_allowed(const mtkx_peg_matcher *matcher,
                                               const char *channel);

/**
 * @brief Enqueues an arm and tries the accumulated arms at the current
 *        position (C++ `matcher << arm`).
 *
 * @IGNORE input is skipped first. The first rule/channel arm that matches
 * fires and consumes its match; otherwise the first wildcard arm fires and
 * consumes one character; otherwise the arms stay queued for the next
 * feed. Arms are cleared whenever one fires.
 */
MTKX_API mtkx_status mtkx_peg_matcher_feed(mtkx_peg_matcher *matcher, mtkx_peg_arm arm);

/** @return the number of arms queued and not yet fired. */
MTKX_API size_t mtkx_peg_matcher_pending_arms(const mtkx_peg_matcher *matcher);

/* ------------------------------------------------------------------ */
/*  PEG combinators (values are MTKX_PV_MATCH unless noted)           */
/* ------------------------------------------------------------------ */

/** Sequence of @p count parsers; spans concatenate (C++ `peg_seq`). */
MTKX_API mtkx_parser *mtkx_peg_seq(size_t count, ...);

/** Ordered choice: first successful alternative wins (C++ `peg_choice`). */
MTKX_API mtkx_parser *mtkx_peg_choice(size_t count, ...);

/** `a?`: never fails; optional value (C++ `peg_opt`). */
MTKX_API mtkx_parser *mtkx_peg_opt(mtkx_parser *p);

/** `a*`: greedy list (C++ `peg_many`). */
MTKX_API mtkx_parser *mtkx_peg_many(mtkx_parser *p);

/** `a+`: pair of first value and list of the rest (C++ `peg_many1`). */
MTKX_API mtkx_parser *mtkx_peg_many1(mtkx_parser *p);

/** `&a`: succeeds with a bool value if a matches; consumes nothing. */
MTKX_API mtkx_parser *mtkx_peg_and(mtkx_parser *p);

/** `!a`: succeeds with a bool value if a fails; consumes nothing. */
MTKX_API mtkx_parser *mtkx_peg_not(mtkx_parser *p);

/** Adapts a rule (which must outlive the parser) honouring must_fail. */
MTKX_API mtkx_parser *mtkx_peg_rule_parser(const mtkx_peg_rule *rule);

/**
 * @brief Tags the span produced by @p p with @p channel (validated; NULL
 *        is returned and @p p released if invalid) (C++ `peg_channel`).
 */
MTKX_API mtkx_parser *mtkx_peg_channel(const char *channel, mtkx_parser *p);

/** Converts any parser into one producing its consumed span. */
MTKX_API mtkx_parser *mtkx_peg_as_span(mtkx_parser *p);

MTKX_END_DECLS

#endif /* MEMTKX_DSL_PEG_H */
