#pragma once
/**
 * @file memtkx/dsl/core.hpp
 * @brief C++ DSL composition: FixedString, the CRTP `DSL<Derived, Features...>`
 *        base, feature tags, predicates and pipelines.
 *
 * These are compile-time C++ idioms layered over the C toolkit; the C
 * counterparts are mtkx_predicate / mtkx_pipe (memtkx/dsl/pipeline.h) and
 * the feature flags in memtkx/dsl/features.h.
 */

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "memtkx/detail/interop.hpp"
#include "memtkx/dsl/features.h"
#include "memtkx/dsl/pipeline.h"

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
#define MEMTKX_CPP20 1
#include <concepts>
#else
#define MEMTKX_CPP20 0
#endif

namespace MemTKX::dsl {

#if MEMTKX_CPP20
/**
 * @brief Compile-time string usable as a template argument (C++20):
 *        `leaf<"tag">(v)`, `pattern<"[0-9]+">`, `rule<"name">(...)`.
 */
template <std::size_t N>
struct FixedString {
  char data[N]{};
  constexpr FixedString(const char (&s)[N]) { std::copy_n(s, N, data); }
  [[nodiscard]] constexpr std::string_view view() const { return {data, N - 1}; }
  constexpr bool operator==(const FixedString&) const = default;
};
#endif

/* ------------------------------------------------------------------ */
/*  Feature tags                                                      */
/* ------------------------------------------------------------------ */

struct Pipeline;
struct Operators;
struct PatternMatch;
struct AST;
struct Rewrite;
struct ExprTemplates;
struct CustomLiterals;
struct Memoization;
struct LazyFeature;
struct Monadic;
struct ResultFeature;
struct CombinatorParser;
struct TaskPipeline;

/** CRTP base composing feature mixins (C: plain structs + feature flags). */
template <typename Derived, typename... Features>
struct DSL : Features::template Mixin<Derived>... {
 protected:
  constexpr Derived& self() noexcept { return static_cast<Derived&>(*this); }
  constexpr const Derived& self() const noexcept {
    return static_cast<const Derived&>(*this);
  }
};

/** Maps a feature tag to its C flag (memtkx/dsl/features.h). */
template <typename Feature>
struct feature_flag;
#define MEMTKX_DSL_FEATURE_FLAG_(Tag, Flag)                     \
  template <>                                                   \
  struct feature_flag<Tag> {                                    \
    static constexpr unsigned value = static_cast<unsigned>(Flag); \
  }
MEMTKX_DSL_FEATURE_FLAG_(Pipeline, MTKX_FEATURE_PIPELINE);
MEMTKX_DSL_FEATURE_FLAG_(Operators, MTKX_FEATURE_OPERATORS);
MEMTKX_DSL_FEATURE_FLAG_(PatternMatch, MTKX_FEATURE_PATTERN_MATCH);
MEMTKX_DSL_FEATURE_FLAG_(AST, MTKX_FEATURE_AST);
MEMTKX_DSL_FEATURE_FLAG_(Rewrite, MTKX_FEATURE_REWRITE);
MEMTKX_DSL_FEATURE_FLAG_(ExprTemplates, MTKX_FEATURE_EXPR_TEMPLATES);
MEMTKX_DSL_FEATURE_FLAG_(CustomLiterals, MTKX_FEATURE_CUSTOM_LITERALS);
MEMTKX_DSL_FEATURE_FLAG_(Memoization, MTKX_FEATURE_MEMOIZATION);
MEMTKX_DSL_FEATURE_FLAG_(LazyFeature, MTKX_FEATURE_LAZY);
MEMTKX_DSL_FEATURE_FLAG_(Monadic, MTKX_FEATURE_MONADIC);
MEMTKX_DSL_FEATURE_FLAG_(ResultFeature, MTKX_FEATURE_RESULT);
MEMTKX_DSL_FEATURE_FLAG_(CombinatorParser, MTKX_FEATURE_COMBINATOR_PARSER);
MEMTKX_DSL_FEATURE_FLAG_(TaskPipeline, MTKX_FEATURE_TASK_PIPELINE);
#undef MEMTKX_DSL_FEATURE_FLAG_

/** The C feature bitmask of a feature list. */
template <typename... Features>
inline constexpr unsigned feature_mask = (0u | ... | feature_flag<Features>::value);

/* ------------------------------------------------------------------ */
/*  Predicates                                                        */
/* ------------------------------------------------------------------ */

/** A composable predicate (`&`, `|`, `!`) with short-circuit evaluation. */
template <typename F>
struct Predicate {
  F fn;
  explicit constexpr Predicate(F f) : fn(std::move(f)) {}

  template <typename T>
  constexpr bool operator()(T&& v) const {
    return fn(std::forward<T>(v));
  }

  template <typename G>
  constexpr auto operator&(Predicate<G> other) const {
    auto combined = [a = fn, b = other.fn](auto&& v) { return a(v) && b(v); };
    return Predicate<decltype(combined)>{std::move(combined)};
  }

  template <typename G>
  constexpr auto operator|(Predicate<G> other) const {
    auto combined = [a = fn, b = other.fn](auto&& v) { return a(v) || b(v); };
    return Predicate<decltype(combined)>{std::move(combined)};
  }

  constexpr auto operator!() const {
    auto negated = [a = fn](auto&& v) { return !a(v); };
    return Predicate<decltype(negated)>{std::move(negated)};
  }
};

/** Wraps a callable as a Predicate. */
template <typename F>
constexpr auto predicate(F&& f) {
  return Predicate<std::decay_t<F>>{std::forward<F>(f)};
}

/* ------------------------------------------------------------------ */
/*  Pipelines                                                         */
/* ------------------------------------------------------------------ */

/** A pipeline stage for `value | pipe(f)`. */
template <typename F>
struct PipeStage {
  F fn;
  explicit constexpr PipeStage(F f) : fn(std::move(f)) {}
};

/** Wraps a callable as a pipeline stage. */
template <typename F>
constexpr auto pipe(F&& f) {
  return PipeStage<std::decay_t<F>>{std::forward<F>(f)};
}

/** `value | stage` invokes the stage on the value. */
template <typename T, typename F>
constexpr auto operator|(T&& val, PipeStage<F> stage) {
  return stage.fn(std::forward<T>(val));
}

/** Feature: `wrap(v)` lifts a value into a pipeline. */
struct Pipeline {
  template <typename Derived>
  struct Mixin {
    template <typename T>
    static constexpr T wrap(T&& v) {
      return std::forward<T>(v);
    }
  };
};

/** Feature: `make_pred(f)` builds a composable predicate. */
struct Operators {
  template <typename Derived>
  struct Mixin {
    template <typename F>
    static constexpr auto make_pred(F&& f) {
      return dsl::predicate(std::forward<F>(f));
    }
  };
};

/** Marker features whose behaviour lives in free functions/classes. */
struct PatternMatch {
  template <typename Derived>
  struct Mixin {};
};
struct LazyFeature {
  template <typename Derived>
  struct Mixin {};
};
struct Monadic {
  template <typename Derived>
  struct Mixin {};
};
struct ResultFeature {
  template <typename Derived>
  struct Mixin {};
};
struct CombinatorParser {
  template <typename Derived>
  struct Mixin {};
};
struct TaskPipeline {
  template <typename Derived>
  struct Mixin {};
};

/** Feature: `clear_cache()` clears `Derived::memo_cache`. */
struct Memoization {
  template <typename Derived>
  struct Mixin {
    void clear_cache() { static_cast<Derived&>(*this).memo_cache.clear(); }
  };
};

}  // namespace MemTKX::dsl
