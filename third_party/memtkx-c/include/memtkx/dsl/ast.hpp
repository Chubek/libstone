#pragma once
/**
 * @file memtkx/dsl/ast.hpp
 * @brief `ASTNode`, `leaf`, `node`, rewrite rules and rewrite sets, built on
 *        the C AST (memtkx/dsl/ast.h).
 *
 * An ASTNode owns a C tree. `children()` / `child(i)` return borrowed
 * views of the C children (cached lazily): read them, copy them (a copy
 * is an independent deep clone), but do not keep references across
 * `append_child` / `take_child` on the parent or after the parent dies.
 */

#include <cstddef>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "memtkx/dsl/ast.h"
#include "memtkx/dsl/core.hpp"

namespace MemTKX::dsl {

class ASTNode {
 public:
  /** An empty leaf with tag "". */
  ASTNode() : node_(checked(mtkx_ast_empty())) {}

  /** A leaf node. */
  ASTNode(std::string_view tag, std::string value)
      : node_(checked(mtkx_ast_leaf(std::string(tag).c_str(), value.c_str()))) {}

  /** An inner node owning @p children. */
  ASTNode(std::string_view tag, std::vector<ASTNode> children) {
    std::vector<mtkx_ast_node*> raw;
    raw.reserve(children.size());
    for (auto& child : children) {
      raw.push_back(child.owned_ ? child.release() : mtkx_ast_clone(child.node_));
    }
    node_ = checked(mtkx_ast_node_new(std::string(tag).c_str(), raw.size(), raw.data()));
  }

  /** Deep copy (always owning, even when copying a borrowed view). */
  ASTNode(const ASTNode& other) : node_(checked(mtkx_ast_clone(other.node_))) {}

  ASTNode(ASTNode&& other) noexcept
      : node_(std::exchange(other.node_, nullptr)),
        owned_(other.owned_),
        children_cache_(std::move(other.children_cache_)),
        children_valid_(std::exchange(other.children_valid_, false)),
        value_cache_(std::move(other.value_cache_)) {}

  ASTNode& operator=(const ASTNode& other) {
    if (this != &other) {
      ASTNode copy(other);
      swap(copy);
    }
    return *this;
  }

  ASTNode& operator=(ASTNode&& other) noexcept {
    if (this != &other) {
      ASTNode moved(std::move(other));
      swap(moved);
    }
    return *this;
  }

  ~ASTNode() {
    if (owned_ && node_ != nullptr) {
      mtkx_ast_free(node_);
    }
  }

  void swap(ASTNode& other) noexcept {
    std::swap(node_, other.node_);
    std::swap(owned_, other.owned_);
    std::swap(children_cache_, other.children_cache_);
    std::swap(children_valid_, other.children_valid_);
    std::swap(value_cache_, other.value_cache_);
  }

  [[nodiscard]] std::string_view tag() const { return mtkx_ast_tag(node_); }
  [[nodiscard]] bool is_leaf() const { return mtkx_ast_is_leaf(node_); }

  /** The leaf value ("" for inner nodes). */
  [[nodiscard]] const std::string& value() const {
    if (!value_cache_) {
      value_cache_.emplace(mtkx_ast_value(node_));
    }
    return *value_cache_;
  }

  /** Borrowed views of the children. */
  [[nodiscard]] const std::vector<ASTNode>& children() const {
    if (!children_valid_) {
      children_cache_.clear();
      const std::size_t n = mtkx_ast_size(node_);
      children_cache_.reserve(n);
      for (std::size_t i = 0; i < n; ++i) {
        children_cache_.push_back(ASTNode(borrow_tag{}, mtkx_ast_child(node_, i)));
      }
      children_valid_ = true;
    }
    return children_cache_;
  }

  /** Child @p i; throws std::out_of_range. */
  [[nodiscard]] const ASTNode& child(std::size_t i) const {
    if (i >= size()) {
      throw std::out_of_range("dsl::ASTNode::child: index out of range");
    }
    return children()[i];
  }

  [[nodiscard]] std::size_t size() const { return mtkx_ast_size(node_); }

  /** Appends a child to an inner node; throws std::logic_error for leaves. */
  void append_child(ASTNode child) {
    mtkx_ast_node* raw = child.owned_ ? child.release() : checked(mtkx_ast_clone(child.node_));
    const mtkx_status status = mtkx_ast_append_child(node_, raw);
    invalidate();
    if (status == MTKX_ERR_INVALID_ARGUMENT) {
      throw std::logic_error("dsl::ASTNode::append_child: leaves have no children");
    }
    detail::throw_if_nomem(status);
  }

  /** Removes and returns child @p i; throws std::out_of_range. */
  ASTNode take_child(std::size_t i) {
    mtkx_ast_node* raw = mtkx_ast_take_child(node_, i);
    if (raw == nullptr) {
      throw std::out_of_range("dsl::ASTNode::take_child: index out of range");
    }
    invalidate();
    return adopt(raw);
  }

  /** S-expression: "(tag value)" or "(tag child ...)". */
  [[nodiscard]] std::string dump() const {
    char* text = mtkx_ast_dump(node_);
    if (text == nullptr) {
      throw std::bad_alloc();
    }
    std::string out(text);
    mtkx_free(text);
    return out;
  }

  bool operator==(const ASTNode& other) const { return mtkx_ast_equal(node_, other.node_); }
  bool operator!=(const ASTNode& other) const { return !(*this == other); }

  /* ---- C interop ---- */

  /** The underlying C node (owned by this object or its root). */
  [[nodiscard]] const mtkx_ast_node* c_node() const noexcept { return node_; }

  /** Takes ownership of a C tree (throws std::bad_alloc for NULL). */
  static ASTNode adopt(mtkx_ast_node* node) { return ASTNode(adopt_tag{}, checked(node)); }

  /** A temporary, non-owning view of a C node (for callbacks). */
  static ASTNode borrow(const mtkx_ast_node* node) {
    return ASTNode(borrow_tag{}, const_cast<mtkx_ast_node*>(node));
  }

  /** Releases ownership of the C tree to the caller (owning nodes only). */
  [[nodiscard]] mtkx_ast_node* release() {
    if (!owned_) {
      return checked(mtkx_ast_clone(node_));
    }
    invalidate();
    return std::exchange(node_, nullptr);
  }

  [[nodiscard]] bool owns() const noexcept { return owned_; }

 private:
  struct borrow_tag {};
  struct adopt_tag {};
  ASTNode(borrow_tag, mtkx_ast_node* node) noexcept : node_(node), owned_(false) {}
  ASTNode(adopt_tag, mtkx_ast_node* node) noexcept : node_(node) {}

  static mtkx_ast_node* checked(mtkx_ast_node* node) {
    if (node == nullptr) {
      throw std::bad_alloc();
    }
    return node;
  }

  void invalidate() noexcept {
    children_cache_.clear();
    children_valid_ = false;
    value_cache_.reset();
  }

  mtkx_ast_node* node_{nullptr};
  bool owned_{true};
  mutable std::vector<ASTNode> children_cache_{};
  mutable bool children_valid_{false};
  mutable std::optional<std::string> value_cache_{};
};

namespace detail {
template <typename T>
std::string to_leaf_text(T&& val) {
  if constexpr (std::is_convertible_v<T, std::string_view>) {
    return std::string(std::string_view(val));
  } else {
    std::ostringstream os;
    os << std::forward<T>(val);
    return os.str();
  }
}
}  // namespace detail

/** Leaf with a runtime tag; the value is formatted with operator<<. */
template <typename T>
ASTNode leaf(std::string_view tag, T&& val) {
  return ASTNode{tag, detail::to_leaf_text(std::forward<T>(val))};
}

/** Inner node with a runtime tag. */
template <typename... Children>
ASTNode node(std::string_view tag, Children&&... children) {
  std::vector<ASTNode> v;
  v.reserve(sizeof...(children));
  (v.emplace_back(std::forward<Children>(children)), ...);
  return ASTNode{tag, std::move(v)};
}

#if MEMTKX_CPP20
/** `leaf<"num">(42)` */
template <FixedString Tag, typename T>
ASTNode leaf(T&& val) {
  return ASTNode{Tag.view(), detail::to_leaf_text(std::forward<T>(val))};
}

/** `node<"add">(a, b)` */
template <FixedString Tag, typename... Children>
  requires(std::convertible_to<Children, ASTNode> && ...)
ASTNode node(Children&&... children) {
  return dsl::node(Tag.view(), std::forward<Children>(children)...);
}
#endif

/** Feature: AST helpers on the DSL type. */
struct AST {
  template <typename Derived>
  struct Mixin {
    template <typename T>
    static ASTNode make_leaf(std::string_view tag, T&& val) {
      return dsl::leaf(tag, std::forward<T>(val));
    }
    template <typename... Children>
    static ASTNode make_node(std::string_view tag, Children&&... children) {
      return dsl::node(tag, std::forward<Children>(children)...);
    }
#if MEMTKX_CPP20
    template <FixedString Tag, typename T>
    static ASTNode make_leaf(T&& val) {
      return dsl::leaf<Tag>(std::forward<T>(val));
    }
    template <FixedString Tag, typename... Children>
    static ASTNode make_node(Children&&... children) {
      return dsl::node<Tag>(std::forward<Children>(children)...);
    }
#endif
  };
};

/* ------------------------------------------------------------------ */
/*  Rewrite                                                           */
/* ------------------------------------------------------------------ */

/** A named rule: `pred(const ASTNode&) -> bool`, `trans(const ASTNode&) -> ASTNode`. */
template <typename Pred, typename Trans>
struct RewriteRule {
  std::string_view name;
  Pred pred;
  Trans trans;

  RewriteRule(std::string_view n, Pred p, Trans t)
      : name(n), pred(std::move(p)), trans(std::move(t)) {}

  [[nodiscard]] std::optional<ASTNode> try_apply(const ASTNode& n) const {
    if (pred(n)) {
      return ASTNode(trans(n));
    }
    return std::nullopt;
  }
};

/** Rule with a runtime name (the name's storage must outlive the rule). */
template <typename Pred, typename Trans>
auto rule(std::string_view name, Pred&& p, Trans&& t) {
  return RewriteRule<std::decay_t<Pred>, std::decay_t<Trans>>{name, std::forward<Pred>(p),
                                                              std::forward<Trans>(t)};
}

#if MEMTKX_CPP20
/** `rule<"name">(pred, trans)` */
template <FixedString Tag, typename Pred, typename Trans>
auto rule(Pred&& p, Trans&& t) {
  return dsl::rule(Tag.view(), std::forward<Pred>(p), std::forward<Trans>(t));
}
#endif

/** Rules applied bottom-up until a fixpoint, via mtkx_rewrite_apply(). */
template <typename... Rules>
struct RewriteSet {
  std::tuple<Rules...> rules;
  std::size_t max_iterations = MTKX_REWRITE_DEFAULT_MAX_ITERATIONS;

  explicit RewriteSet(Rules... rs) : rules(std::move(rs)...) {}

  [[nodiscard]] ASTNode apply(const ASTNode& root) const {
    mtkx_rewrite_rule c_rules[sizeof...(Rules) == 0 ? 1 : sizeof...(Rules)];
    std::size_t i = 0;
    std::apply(
        [&](const auto&... r) {
          ((c_rules[i++] = make_c_rule(r)), ...);
        },
        rules);
    mtkx_rewrite_set set = mtkx_rewrite_set_make(c_rules, sizeof...(Rules));
    set.max_iterations = max_iterations;
    mtkx_ast_node* out = mtkx_rewrite_apply(&set, root.c_node());
    if (out == nullptr) {
      detail::rethrow_pending();
      throw std::bad_alloc();
    }
    ASTNode result = ASTNode::adopt(out); /* owned before any rethrow */
    detail::rethrow_pending();
    return result;
  }

 private:
  template <typename R>
  static bool pred_trampoline(const mtkx_ast_node* node, void* ctx) {
    if (detail::has_pending_exception()) {
      return false;
    }
    try {
      const ASTNode view = ASTNode::borrow(node);
      return static_cast<const R*>(ctx)->pred(view);
    } catch (...) {
      detail::park_current_exception();
      return false;
    }
  }

  template <typename R>
  static mtkx_ast_node* trans_trampoline(const mtkx_ast_node* node, void* ctx) {
    try {
      const ASTNode view = ASTNode::borrow(node);
      ASTNode out(static_cast<const R*>(ctx)->trans(view));
      return out.release();
    } catch (...) {
      detail::park_current_exception();
      return nullptr;
    }
  }

  template <typename R>
  static mtkx_rewrite_rule make_c_rule(const R& r) {
    return mtkx_rewrite_rule_make(nullptr, &pred_trampoline<R>, &trans_trampoline<R>,
                                  const_cast<R*>(&r));
  }
};

template <typename... Rules>
auto rewrite_set(Rules&&... rs) {
  return RewriteSet<std::decay_t<Rules>...>{std::forward<Rules>(rs)...};
}

/** Feature: `rewrite(tree)` applies `Derived::rules`. */
struct Rewrite {
  template <typename Derived>
  struct Mixin {
    ASTNode rewrite(const ASTNode& tree) const { return Derived::rules.apply(tree); }
  };
};

}  // namespace MemTKX::dsl
