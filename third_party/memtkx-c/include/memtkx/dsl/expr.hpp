#pragma once
/**
 * @file memtkx/dsl/expr.hpp
 * @brief Lazy expression templates for C++ value types (C counterpart for
 *        doubles and arrays: memtkx/dsl/expr.h).
 */

#include <functional>
#include <type_traits>
#include <utility>

#include "memtkx/dsl/core.hpp"
#include "memtkx/dsl/expr.h"

namespace MemTKX::dsl {

struct ExprTerminal {};

namespace detail {
template <typename T, typename = void>
struct has_eval : std::false_type {};
template <typename T>
struct has_eval<T, std::void_t<decltype(std::declval<const T&>().eval())>> : std::true_type {};
template <typename T, typename = void>
struct has_expr_value : std::false_type {};
template <typename T>
struct has_expr_value<T, std::void_t<decltype(std::declval<const T&>().expr_value())>>
    : std::true_type {};

template <typename T>
constexpr auto eval_operand(const T& operand) {
  if constexpr (has_eval<T>::value) {
    return operand.eval();
  } else if constexpr (has_expr_value<T>::value) {
    return operand.expr_value();
  } else {
    return operand;
  }
}
}  // namespace detail

/** Binary node; evaluation is deferred until eval(). */
template <typename Op, typename L, typename R>
struct BinExpr {
  L lhs;
  R rhs;
  constexpr BinExpr(L l, R r) : lhs(std::move(l)), rhs(std::move(r)) {}
  constexpr auto eval() const {
    auto l = detail::eval_operand(lhs);
    auto r = detail::eval_operand(rhs);
    return Op{}(l, r);
  }
};

/** Unary node. */
template <typename Op, typename E>
struct UnaryExpr {
  E operand;
  explicit constexpr UnaryExpr(E e) : operand(std::move(e)) {}
  constexpr auto eval() const { return Op{}(detail::eval_operand(operand)); }
};

/** Feature: arithmetic operators on `Derived` building lazy trees. */
struct ExprTemplates {
  template <typename Derived>
  struct Mixin {
    template <typename R>
    friend constexpr auto operator+(const Derived& lhs, const R& rhs) {
      return BinExpr<std::plus<>, Derived, R>{lhs, rhs};
    }
    template <typename R>
    friend constexpr auto operator-(const Derived& lhs, const R& rhs) {
      return BinExpr<std::minus<>, Derived, R>{lhs, rhs};
    }
    template <typename R>
    friend constexpr auto operator*(const Derived& lhs, const R& rhs) {
      return BinExpr<std::multiplies<>, Derived, R>{lhs, rhs};
    }
    template <typename R>
    friend constexpr auto operator/(const Derived& lhs, const R& rhs) {
      return BinExpr<std::divides<>, Derived, R>{lhs, rhs};
    }
    friend constexpr auto operator-(const Derived& e) {
      return UnaryExpr<std::negate<>, Derived>{e};
    }
  };
};

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto operator+(const BinExpr<Op1, L1, R1>& lhs, const R& rhs) {
  return BinExpr<std::plus<>, BinExpr<Op1, L1, R1>, R>{lhs, rhs};
}
template <typename Op1, typename L1, typename R1, typename R>
constexpr auto operator-(const BinExpr<Op1, L1, R1>& lhs, const R& rhs) {
  return BinExpr<std::minus<>, BinExpr<Op1, L1, R1>, R>{lhs, rhs};
}
template <typename Op1, typename L1, typename R1, typename R>
constexpr auto operator*(const BinExpr<Op1, L1, R1>& lhs, const R& rhs) {
  return BinExpr<std::multiplies<>, BinExpr<Op1, L1, R1>, R>{lhs, rhs};
}
template <typename Op1, typename L1, typename R1, typename R>
constexpr auto operator/(const BinExpr<Op1, L1, R1>& lhs, const R& rhs) {
  return BinExpr<std::divides<>, BinExpr<Op1, L1, R1>, R>{lhs, rhs};
}

}  // namespace MemTKX::dsl
