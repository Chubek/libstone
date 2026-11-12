#pragma once
/**
 * @file memtkx/dsl/match.hpp
 * @brief Dispatch tables (`match(when<K>(h), ..., otherwise(h))`) and the
 *        regex-subset patterns, which delegate to mtkx_pattern_matches_n().
 */

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "memtkx/dsl/core.hpp"
#include "memtkx/dsl/match.h"

namespace MemTKX::dsl {

/** Runtime regex-subset test (anchored; see mtkx_pattern_matches). */
inline bool pattern_matches(std::string_view pattern, std::string_view input) noexcept {
  return mtkx_pattern_matches_n(pattern.data(), pattern.size(), input.data(), input.size());
}

/** A runtime pattern object usable wherever a pattern tag is. */
class Pattern {
 public:
  explicit Pattern(std::string pattern) : pattern_(std::move(pattern)) {}
  [[nodiscard]] bool matches(std::string_view input) const noexcept {
    return pattern_matches(pattern_, input);
  }
  [[nodiscard]] const std::string& value() const noexcept { return pattern_; }

 private:
  std::string pattern_;
};

#if MEMTKX_CPP20
/** Compile-time pattern tag: `when<pattern<"[0-9]+">{}>(handler)`. */
template <FixedString S>
struct pattern {
  static constexpr std::string_view value = S.view();
  static bool matches(std::string_view input) noexcept { return pattern_matches(value, input); }
};
#endif

namespace detail {
template <typename K, typename = void>
struct has_static_matches : std::false_type {};
template <typename K>
struct has_static_matches<K, std::void_t<decltype(K::matches(std::string_view{}))>>
    : std::true_type {};

template <typename A, typename B, typename = void>
struct equality_comparable_with : std::false_type {};
template <typename A, typename B>
struct equality_comparable_with<A, B,
                                std::void_t<decltype(std::declval<const A&>() ==
                                                     std::declval<const B&>())>>
    : std::true_type {};
}  // namespace detail

/** One value or pattern clause (C++17: any `auto` non-type key). */
template <auto Key, typename F>
struct WhenClause {
  F handler;
  using handler_type_or_void = F;
  explicit constexpr WhenClause(F f) : handler(std::move(f)) {}

  template <typename K, typename R, typename... Args>
  constexpr bool try_invoke(K key, std::optional<R>& out, Args&&... args) const {
    using KeyT = std::remove_cv_t<std::remove_reference_t<decltype(Key)>>;
    if constexpr (detail::has_static_matches<KeyT>::value) {
      if constexpr (std::is_convertible_v<K, std::string_view>) {
        if (KeyT::matches(std::string_view(key))) {
          out = handler(key, std::forward<Args>(args)...);
          return true;
        }
      }
    } else if constexpr (detail::equality_comparable_with<K, KeyT>::value) {
      if (key == Key) {
        out = handler(key, std::forward<Args>(args)...);
        return true;
      }
    }
    return false;
  }
};

template <auto Key, typename F>
constexpr auto when(F&& f) {
  return WhenClause<Key, std::decay_t<F>>{std::forward<F>(f)};
}

/** Fallback clause. */
template <typename F>
struct OtherwiseClause {
  F handler;
  using handler_type_or_void = F;
  explicit constexpr OtherwiseClause(F f) : handler(std::move(f)) {}

  template <typename K, typename R, typename... Args>
  constexpr bool try_invoke(K, std::optional<R>&, Args&&...) const {
    return false;
  }
};

template <typename F>
constexpr auto otherwise(F&& f) {
  return OtherwiseClause<std::decay_t<F>>{std::forward<F>(f)};
}

namespace detail {
template <typename T>
struct is_otherwise : std::false_type {};
template <typename F>
struct is_otherwise<OtherwiseClause<F>> : std::true_type {};
}  // namespace detail

/** A dispatch table; throws std::runtime_error when nothing matches. */
template <typename... Clauses>
struct MatchTable {
  std::tuple<Clauses...> clauses;
  explicit constexpr MatchTable(Clauses... cs) : clauses(std::move(cs)...) {}

  template <typename Key, typename... Args>
  constexpr auto operator()(Key key, Args&&... args) const {
    using RetT = std::common_type_t<
        std::invoke_result_t<typename Clauses::handler_type_or_void, Key, Args...>...>;
    std::optional<RetT> result;
    bool has_otherwise = false;
    const bool matched = std::apply(
        [&](const auto&... c) { return (c.try_invoke(key, result, args...) || ...); },
        clauses);
    if (!matched) {
      std::apply(
          [&](const auto&... c) {
            (
                [&] {
                  using Cls = std::remove_cv_t<std::remove_reference_t<decltype(c)>>;
                  if constexpr (detail::is_otherwise<Cls>::value) {
                    has_otherwise = true;
                    if (!result) {
                      result = c.handler(key, args...);
                    }
                  }
                }(),
                ...);
          },
          clauses);
    }
    if (!result && !has_otherwise) {
      throw std::runtime_error("dsl::match: no clause matched and no otherwise");
    }
    return *result;
  }
};

template <typename... Cs>
MatchTable(Cs...) -> MatchTable<Cs...>;

template <typename... Clauses>
constexpr auto match(Clauses&&... cs) {
  return MatchTable<std::decay_t<Clauses>...>{std::forward<Clauses>(cs)...};
}

}  // namespace MemTKX::dsl
