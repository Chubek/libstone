#pragma once
/**
 * @file memtkx/dsl/result.hpp
 * @brief `Result<T, E>`, `Maybe<T>`, optional helpers, `Lazy<T>` and
 *        `memoize` for C++ (C counterparts: MTKX_DEFINE_RESULT,
 *        MTKX_DEFINE_MAYBE, mtkx_lazy, mtkx_memo).
 */

#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "memtkx/dsl/core.hpp"

namespace MemTKX::dsl {

/**
 * @brief Explicit success/error value.
 *
 * Unlike the original variant-based version, the active branch is a flag,
 * so `Result<T, T>` works, and `unwrap_err()` gives direct error access.
 */
template <typename T, typename E>
class Result {
 public:
  /** Ok result (implicit, as in the original). */
  Result(T value) : value_(std::in_place, std::move(value)) {}

  /** Err result (only when T and E differ, to avoid ambiguity). */
  template <typename U = E,
            std::enable_if_t<!std::is_same_v<T, U> && std::is_same_v<U, E>, int> = 0>
  Result(E error) : error_(std::in_place, std::move(error)) {}

  [[nodiscard]] bool is_ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] bool is_err() const noexcept { return !value_.has_value(); }

  /** @return the value; throws std::runtime_error on Err. */
  [[nodiscard]] T unwrap() const {
    if (!is_ok()) {
      throw std::runtime_error("dsl::Result::unwrap called on Err");
    }
    return *value_;
  }

  /** @return the error; throws std::runtime_error on Ok. */
  [[nodiscard]] E unwrap_err() const {
    if (is_ok()) {
      throw std::runtime_error("dsl::Result::unwrap_err called on Ok");
    }
    return *error_;
  }

  [[nodiscard]] T unwrap_or(T fallback) const {
    return is_ok() ? *value_ : std::move(fallback);
  }

  template <typename Fn>
  auto map(Fn&& fn) const -> Result<decltype(fn(std::declval<T>())), E> {
    using U = decltype(fn(std::declval<T>()));
    if (is_ok()) {
      return Result<U, E>::from_ok(fn(*value_));
    }
    return Result<U, E>::from_err(*error_);
  }

  template <typename Fn>
  auto and_then(Fn&& fn) const -> decltype(fn(std::declval<T>())) {
    using R = decltype(fn(std::declval<T>()));
    if (is_ok()) {
      return fn(*value_);
    }
    return R::from_err(*error_);
  }

  template <typename Fn>
  auto map_err(Fn&& fn) const -> Result<T, decltype(fn(std::declval<E>()))> {
    using E2 = decltype(fn(std::declval<E>()));
    if (is_ok()) {
      return Result<T, E2>::from_ok(*value_);
    }
    return Result<T, E2>::from_err(fn(*error_));
  }

  static Result from_ok(T value) { return Result(std::move(value)); }
  static Result from_err(E error) { return Result(err_tag{}, std::move(error)); }

 private:
  struct err_tag {};
  Result(err_tag, E error) : error_(std::in_place, std::move(error)) {}

  std::optional<T> value_{};
  std::optional<E> error_{};
};

template <typename T>
struct OkWrap {
  T value;
};
template <typename E>
struct ErrWrap {
  E error;
};
template <typename T>
OkWrap<T> Ok(T value) {
  return {std::move(value)};
}
template <typename E>
ErrWrap<E> Err(E error) {
  return {std::move(error)};
}

/** Optional value with fluent monadic helpers. */
template <typename T>
class Maybe {
 public:
  Maybe() = default;
  Maybe(T value) : value_(std::move(value)) {}
  Maybe(std::optional<T> value) : value_(std::move(value)) {}

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] const std::optional<T>& as_optional() const { return value_; }

  template <typename Fn>
  auto map(Fn&& fn) const -> Maybe<decltype(fn(std::declval<T>()))> {
    using U = decltype(fn(std::declval<T>()));
    if (!value_) {
      return Maybe<U>{};
    }
    return Maybe<U>{fn(*value_)};
  }

  template <typename Fn>
  auto flat_map(Fn&& fn) const -> decltype(fn(std::declval<T>())) {
    using R = decltype(fn(std::declval<T>()));
    if (!value_) {
      return R{};
    }
    return fn(*value_);
  }

  template <typename Pred>
  Maybe filter(Pred&& pred) const {
    if (value_ && pred(*value_)) {
      return *this;
    }
    return Maybe{};
  }

  T or_else(T fallback) const { return value_.value_or(std::move(fallback)); }

 private:
  std::optional<T> value_{};
};

template <typename Opt, typename Fn>
auto map(const Opt& opt, Fn&& fn) -> std::optional<decltype(fn(*opt))> {
  if (!opt) {
    return std::nullopt;
  }
  return fn(*opt);
}

template <typename Opt, typename Fn>
auto flat_map(const Opt& opt, Fn&& fn) -> decltype(fn(*opt)) {
  using R = decltype(fn(*opt));
  if (!opt) {
    return R{};
  }
  return fn(*opt);
}

template <typename Opt, typename Pred>
auto filter(const Opt& opt, Pred&& pred) -> Opt {
  if (opt && pred(*opt)) {
    return opt;
  }
  return std::nullopt;
}

template <typename Opt, typename T>
T or_else(const Opt& opt, T fallback) {
  return opt.value_or(std::move(fallback));
}

/** Deferred single computation, cached on first access. */
template <typename T>
class Lazy {
 public:
  explicit Lazy(std::function<T()> producer) : producer_(std::move(producer)) {}
  T& get() {
    if (!value_) {
      value_ = producer_();
    }
    return *value_;
  }
  T& force() { return get(); }
  [[nodiscard]] bool is_computed() const noexcept { return value_.has_value(); }

 private:
  std::function<T()> producer_;
  std::optional<T> value_{};
};

/** Callable with a cache keyed by its argument (not synchronised). */
template <typename Key, typename Value, typename Fn>
class MemoizedCallable {
 public:
  explicit MemoizedCallable(Fn fn) : fn_(std::move(fn)) {}
  Value operator()(const Key& key) {
    if (auto it = cache_.find(key); it != cache_.end()) {
      return it->second;
    }
    auto value = fn_(key);
    cache_.emplace(key, value);
    return value;
  }
  void clear_cache() { cache_.clear(); }
  [[nodiscard]] std::size_t cache_size() const noexcept { return cache_.size(); }

 private:
  Fn fn_;
  std::unordered_map<Key, Value> cache_{};
};

template <typename Key, typename Value, typename Fn>
auto memoize(Fn&& fn) {
  return MemoizedCallable<Key, Value, std::decay_t<Fn>>{std::forward<Fn>(fn)};
}

}  // namespace MemTKX::dsl
