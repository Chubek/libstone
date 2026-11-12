#pragma once
/**
 * @file memtkx/dsl/peg.hpp
 * @brief PEG grammars over the C PEG engine (memtkx/dsl/peg.h):
 *        `PEGRule`, `PEGDefinition`, `PEGMatcher` and the PEG combinators.
 *
 * A `PEGRule`'s public fields (action, channel, must_fail, fire_on_neg,
 * if_succeed) stay freely mutable, as in the original. They are flushed
 * to the C rule whenever the definition parses, a matcher is fed, or a
 * rule parser is created.
 */

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "memtkx/dsl/parser.hpp"
#include "memtkx/dsl/peg.h"

namespace MemTKX::dsl {

using PEGSemanticAction = std::function<void(PEGMatch&)>;

namespace peg_detail {
/** A match arm consumed by `PEGMatcher::operator<<`. */
struct PEGArm {
  PEGRule* rule{nullptr};
  PEGSemanticAction action{};
  bool is_wildcard{false};
  bool is_channel{false};
  PEGChannel channel{};
};

inline void action_trampoline(mtkx_peg_match* m, void* ctx) {
  if (MemTKX::detail::has_pending_exception()) {
    return;
  }
  try {
    PEGMatch match = MemTKX::dsl::detail::from_c_match(*m);
    (*static_cast<PEGSemanticAction*>(ctx))(match);
    m->user_data = match.user_data;
  } catch (...) {
    MemTKX::detail::park_current_exception();
  }
}
}  // namespace peg_detail

/** A rule owned by a PEGDefinition. */
struct PEGRule {
  std::string pattern_text{};
  PEGSemanticAction semantic_action{};
  PEGChannel channel{MTKX_PEG_DEFAULT_CHANNEL};
  bool must_fail{false};
  bool fire_on_neg{false};
  PEGRule* if_succeed{nullptr};
  std::size_t order{0};

  PEGRule(const PEGRule&) = delete;
  PEGRule& operator=(const PEGRule&) = delete;

  /** Attempts the rule at @p pos. */
  [[nodiscard]] std::optional<std::size_t> try_match(std::string_view text,
                                                     std::size_t pos) const {
    flush();
    std::size_t end = 0;
    if (mtkx_peg_rule_try_match(c_, text.data(), text.size(), pos, &end)) {
      return end;
    }
    return std::nullopt;
  }

  /** Builds a match arm binding this rule to @p fn. */
  template <typename Fn>
  peg_detail::PEGArm operator()(Fn&& fn) const {
    peg_detail::PEGArm arm;
    arm.rule = const_cast<PEGRule*>(this);
    arm.action = std::forward<Fn>(fn);
    return arm;
  }

  /** The C rule (after synchronising the public fields). */
  [[nodiscard]] mtkx_peg_rule* c_rule() const {
    flush();
    return c_;
  }

  /** Pushes the public fields into the C rule. */
  void flush() const {
    sync_fields();
    if (if_succeed != nullptr && if_succeed != this) {
      if_succeed->sync_fields();
    }
  }

  // construction is reserved for PEGDefinition
  explicit PEGRule(mtkx_peg_rule* c) : pattern_text(c->pattern_text), order(c->order), c_(c) {
    c_->user_data = this;
  }

 private:
  void sync_fields() const {
    c_->must_fail = must_fail;
    c_->fire_on_neg = fire_on_neg;
    c_->if_succeed = if_succeed != nullptr ? if_succeed->c_ : nullptr;
    if (channel.name != c_->channel) {
      const mtkx_status status = mtkx_peg_rule_set_channel(c_, channel.name.c_str());
      if (status == MTKX_ERR_INVALID_ARGUMENT) {
        throw std::invalid_argument("dsl::PEGRule: channel names must begin with '@'");
      }
      MemTKX::detail::throw_if_nomem(status);
    }
    if (semantic_action) {
      mtkx_peg_rule_set_action(c_, &peg_detail::action_trampoline,
                               const_cast<PEGSemanticAction*>(&semantic_action));
    } else {
      mtkx_peg_rule_set_action(c_, nullptr, nullptr);
    }
  }

  mtkx_peg_rule* c_;
};

/** Diagnostics of PEGDefinition::parse. */
struct PEGParseResult {
  bool success{true};
  std::size_t offset{0};
  std::size_t line{1};
  std::size_t column{1};
  std::string message{};

  [[nodiscard]] bool ok() const noexcept { return success; }
  [[nodiscard]] bool failed() const noexcept { return !success; }
  [[nodiscard]] std::size_t error_offset() const noexcept { return offset; }
  [[nodiscard]] std::size_t error_line() const noexcept { return line; }
  [[nodiscard]] std::size_t error_column() const noexcept { return column; }
  [[nodiscard]] std::string_view error_message() const noexcept { return message; }
};

/** An ordered rule set with optional parent. Move-only; rules have stable addresses. */
class PEGDefinition {
 public:
  /** Informational parent link (as in the original). */
  const PEGDefinition* parent{nullptr};

  PEGDefinition() : impl_(std::make_unique<Impl>(mtkx_peg_definition_create())) {}
  PEGDefinition(PEGDefinition&&) noexcept = default;
  PEGDefinition& operator=(PEGDefinition&&) noexcept = default;
  PEGDefinition(const PEGDefinition&) = delete;
  PEGDefinition& operator=(const PEGDefinition&) = delete;

  /** Adds a rule; throws std::invalid_argument for a malformed pattern. */
  PEGRule& add_rule(std::string_view pattern, PEGSemanticAction action = {}) {
    mtkx_peg_rule* c = nullptr;
    const std::string text(pattern);
    const mtkx_status status = mtkx_peg_definition_add_rule(impl_->c, text.c_str(), nullptr,
                                                            nullptr, &c);
    if (status == MTKX_ERR_INVALID_ARGUMENT) {
      const char* why = mtkx_peg_pattern_error(text.c_str());
      throw std::invalid_argument(std::string("dsl::PEGDefinition::add_rule: ") +
                                  (why != nullptr ? why : "invalid pattern") + " in pattern '" +
                                  text + "'");
    }
    MemTKX::detail::throw_if_nomem(status);
    PEGRule& rule = impl_->rules.emplace_back(c);
    rule.semantic_action = std::move(action);
    return rule;
  }

#if MEMTKX_CPP20
  template <FixedString S, typename Fn>
  PEGRule& add_rule(Fn&& action) {
    return add_rule(S.view(), PEGSemanticAction(std::forward<Fn>(action)));
  }
  template <FixedString S>
  PEGRule& add_rule() {
    return add_rule(S.view());
  }
#endif

  /** Visible rules, parent rules first. */
  [[nodiscard]] std::vector<const PEGRule*> rules() const {
    const std::size_t n = mtkx_peg_definition_rules(impl_->c, nullptr, 0);
    std::vector<const mtkx_peg_rule*> raw(n);
    mtkx_peg_definition_rules(impl_->c, raw.data(), n);
    std::vector<const PEGRule*> out;
    out.reserve(n);
    for (const mtkx_peg_rule* r : raw) {
      out.push_back(static_cast<const PEGRule*>(r->user_data));
    }
    return out;
  }

  /** Tokenizes @p text with ordered choice, running semantic actions. */
  [[nodiscard]] PEGParseResult parse(std::string_view text) const {
    flush_all();
    const mtkx_peg_parse_result r = mtkx_peg_definition_parse(impl_->c, text.data(), text.size());
    MemTKX::detail::rethrow_pending();
    return PEGParseResult{r.success, r.offset, r.line, r.column, r.message};
  }

  /** Synchronises every visible rule into C. */
  void flush_all() const {
    for (const PEGRule* rule : rules()) {
      rule->flush();
    }
  }

  [[nodiscard]] mtkx_peg_definition* c_definition() const noexcept { return impl_->c; }

 private:
  friend PEGDefinition derive_peg_definition(const PEGDefinition& parent);

  struct Impl {
    explicit Impl(mtkx_peg_definition* d) : c(d) {
      if (c == nullptr) {
        throw std::bad_alloc();
      }
    }
    ~Impl() { mtkx_peg_definition_destroy(c); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    mtkx_peg_definition* c;
    std::deque<PEGRule> rules;
  };

  explicit PEGDefinition(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

  std::unique_ptr<Impl> impl_;
};

inline PEGDefinition create_peg_definition() { return PEGDefinition{}; }

/** Derives a definition; parent rules stay visible (the parent must outlive it). */
inline PEGDefinition derive_peg_definition(const PEGDefinition& parent) {
  PEGDefinition d(std::make_unique<PEGDefinition::Impl>(
      mtkx_peg_definition_derive(parent.c_definition())));
  d.parent = &parent;
  return d;
}

/** Streaming matcher over borrowed text. */
class PEGMatcher {
 public:
  const PEGDefinition* def{nullptr};
  std::string_view text{};

  PEGMatcher(const PEGDefinition& peg, std::string_view t)
      : def(&peg), text(t), m_(mtkx_peg_matcher_create(peg.c_definition(), t.data(), t.size())) {
    if (!m_) {
      throw std::bad_alloc();
    }
  }

  [[nodiscard]] bool is_spent() const noexcept { return mtkx_peg_matcher_is_spent(m_.get()); }
  [[nodiscard]] std::size_t position() const noexcept {
    return mtkx_peg_matcher_position(m_.get());
  }
  void close() noexcept {}

  PEGMatcher& only(std::string_view channel) {
    MemTKX::detail::throw_if_nomem(mtkx_peg_matcher_only(m_.get(), std::string(channel).c_str()));
    return *this;
  }
  PEGMatcher& include(std::string_view channel) {
    MemTKX::detail::throw_if_nomem(
        mtkx_peg_matcher_include(m_.get(), std::string(channel).c_str()));
    return *this;
  }
  PEGMatcher& exclude(std::string_view channel) {
    MemTKX::detail::throw_if_nomem(
        mtkx_peg_matcher_exclude(m_.get(), std::string(channel).c_str()));
    return *this;
  }
  [[nodiscard]] bool channel_allowed(std::string_view name) const {
    return mtkx_peg_matcher_channel_allowed(m_.get(), std::string(name).c_str());
  }

  template <typename Fn>
  peg_detail::PEGArm wildcard(Fn&& fn) {
    peg_detail::PEGArm arm;
    arm.is_wildcard = true;
    arm.action = std::forward<Fn>(fn);
    return arm;
  }

  /** An arm matching any visible rule on @p channel. */
  template <typename Fn>
  peg_detail::PEGArm on_channel(PEGChannel channel, Fn&& fn) {
    peg_detail::PEGArm arm;
    arm.is_channel = true;
    arm.channel = std::move(channel);
    arm.action = std::forward<Fn>(fn);
    return arm;
  }

  /** Enqueues an arm and tries the queued arms (see mtkx_peg_matcher_feed). */
  PEGMatcher& operator<<(const peg_detail::PEGArm& arm) {
    def->flush_all();
    mtkx_peg_action_fn fn = nullptr;
    void* ctx = nullptr;
    if (arm.action) {
      actions_.push_back(arm.action);
      fn = &peg_detail::action_trampoline;
      ctx = &actions_.back();
    }
    mtkx_peg_arm c_arm;
    if (arm.is_wildcard) {
      c_arm = mtkx_peg_wildcard_arm(fn, ctx);
    } else if (arm.is_channel) {
      c_arm = mtkx_peg_channel_arm(arm.channel.name.c_str(), fn, ctx);
    } else {
      c_arm = mtkx_peg_rule_arm(arm.rule != nullptr ? arm.rule->c_rule() : nullptr, fn, ctx);
    }
    MemTKX::detail::throw_if_nomem(mtkx_peg_matcher_feed(m_.get(), c_arm));
    if (mtkx_peg_matcher_pending_arms(m_.get()) == 0) {
      actions_.clear();
    }
    MemTKX::detail::rethrow_pending();
    return *this;
  }

  [[nodiscard]] mtkx_peg_matcher* c_matcher() const noexcept { return m_.get(); }

 private:
  MemTKX::detail::Handle<mtkx_peg_matcher, &mtkx_peg_matcher_destroy> m_;
  std::deque<PEGSemanticAction> actions_;
};

inline PEGMatcher peg_new_matcher(const PEGDefinition& peg, std::string_view text) {
  return PEGMatcher(peg, text);
}

/* ------------------------------------------------------------------ */
/*  PEG combinators                                                   */
/* ------------------------------------------------------------------ */

namespace peg_detail {
template <typename T>
Parser<PEGMatch> as_span_parser(const Parser<T>& p) {
  return Parser<PEGMatch>(mtkx_peg_as_span(p.retain_c()));
}
}  // namespace peg_detail

/** Sequence; spans concatenate. */
template <typename A, typename B, typename... Rest>
Parser<PEGMatch> peg_seq(const Parser<A>& a, const Parser<B>& b, const Rest&... rest) {
  return Parser<PEGMatch>(
      mtkx_peg_seq(2 + sizeof...(rest), a.retain_c(), b.retain_c(), rest.retain_c()...));
}

/** Ordered choice: the first successful alternative wins. */
template <typename A, typename B, typename... Rest>
Parser<A> peg_choice(const Parser<A>& a, const Parser<B>& b, const Rest&... rest) {
  return Parser<A>(
      mtkx_peg_choice(2 + sizeof...(rest), a.retain_c(), b.retain_c(), rest.retain_c()...));
}

template <typename T>
Parser<std::optional<T>> peg_opt(const Parser<T>& p) {
  return Parser<std::optional<T>>(mtkx_peg_opt(p.retain_c()));
}

template <typename T>
Parser<std::vector<T>> peg_many(const Parser<T>& p) {
  return Parser<std::vector<T>>(mtkx_peg_many(p.retain_c()));
}

template <typename T>
Parser<std::pair<T, std::vector<T>>> peg_many1(const Parser<T>& p) {
  return Parser<std::pair<T, std::vector<T>>>(mtkx_peg_many1(p.retain_c()));
}

template <typename T>
Parser<bool> peg_and(const Parser<T>& p) {
  return Parser<bool>(mtkx_peg_and(p.retain_c()));
}

template <typename T>
Parser<bool> peg_not(const Parser<T>& p) {
  return Parser<bool>(mtkx_peg_not(p.retain_c()));
}

/** Adapts a rule (which must outlive the parser); its fields are flushed now. */
inline Parser<PEGMatch> peg_rule(const PEGRule& rule) {
  return Parser<PEGMatch>(mtkx_peg_rule_parser(rule.c_rule()));
}

/** Tags a parser's span with a channel (the name must begin with '@'). */
template <typename T>
Parser<PEGMatch> peg_channel(PEGChannel channel, const Parser<T>& p) {
  if (mtkx_peg_channel_validate(channel.name.c_str()) != MTKX_OK) {
    throw std::invalid_argument("dsl::peg_channel: channel names must begin with '@'");
  }
  return Parser<PEGMatch>(mtkx_peg_channel(channel.name.c_str(), p.retain_c()));
}

template <typename T>
Parser<PEGMatch> peg_channel(std::string_view name, const Parser<T>& p) {
  return peg_channel(new_peg_channel(name), p);
}

}  // namespace MemTKX::dsl
