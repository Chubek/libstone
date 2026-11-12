#pragma once
/**
 * @file memtkx/MemTKXDSL.hpp
 * @brief C++ wrapper for the MemTKX DSL toolkit (`MemTKX::dsl`).
 *
 * Requires C++17; the string-literal template forms (`leaf<"tag">`,
 * `pattern<"...">`, `rule<"name">`, `lit<"_km">`, `add_rule<"...">`,
 * `production<"id">`) and the concepts need C++20. Runtime-string
 * equivalents are always available (`leaf("tag", v)`, `rule("name", p, t)`,
 * `lit("_km", f)`, `add_rule("pattern", action)`, `Pattern`).
 *
 * Components with runtime behaviour delegate to the C library:
 *
 *   | C++                         | C implementation                 |
 *   |-----------------------------|----------------------------------|
 *   | pattern / Pattern matching  | mtkx_pattern_matches_n           |
 *   | ASTNode, RewriteSet         | mtkx_ast_*, mtkx_rewrite_apply   |
 *   | LiteralSet::parse           | mtkx_literal_set_parse_n         |
 *   | Parser<T> and combinators   | mtkx_parser_*, mtkx_run_parser   |
 *   | PEGDefinition, PEGMatcher   | mtkx_peg_*                       |
 *   | run(TaskState&, TaskChain)  | mtkx_task_run                    |
 *
 * Purely compile-time idioms (DSL<Derived, Features...>, Predicate,
 * pipe, match/when/otherwise, expression templates, Result, Maybe, Lazy,
 * memoize) are implemented as C++ templates.
 */

#include "memtkx/dsl/ast.hpp"
#include "memtkx/dsl/core.hpp"
#include "memtkx/dsl/expr.hpp"
#include "memtkx/dsl/literals.hpp"
#include "memtkx/dsl/match.hpp"
#include "memtkx/dsl/parser.hpp"
#include "memtkx/dsl/peg.hpp"
#include "memtkx/dsl/result.hpp"
#include "memtkx/dsl/task.hpp"

#if MEMTKX_CPP20
namespace MemTKX::dsl {

/** T has feature F mixed in. */
template <typename T, typename F>
concept HasFeature = std::derived_from<T, typename F::template Mixin<T>>;

template <typename T>
concept Pipeable = HasFeature<T, Pipeline>;

template <typename T>
concept ExprLike = HasFeature<T, ExprTemplates> && requires(const T& t) { t.expr_value(); };

template <typename T>
concept HasLiterals = HasFeature<T, CustomLiterals> && requires { T::literals; };

template <typename T>
concept Rewritable = HasFeature<T, Rewrite> && requires { T::rules; };

}  // namespace MemTKX::dsl
#endif
