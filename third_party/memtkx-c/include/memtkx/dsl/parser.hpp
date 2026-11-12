#pragma once
/**
 * @file memtkx/dsl/parser.hpp
 * @brief Typed parser combinators (`Parser<T>`, `ch`, `satisfy`, `&`, `|`,
 *        unary `*`, `optional`, `try_parse`, `labeled`, `run_parser`) over
 *        the C combinator engine (memtkx/dsl/parser.h).
 *
 * A `Parser<T>` holds a reference to a C parser. C values are converted to
 * T on the way out: `char`, `bool`, `std::pair`, `std::vector` and
 * `std::optional` map onto the native C value kinds, and any other T is
 * carried as an owned user value. Parsers from `parser(fn)` run C++ code
 * inside the C engine; exceptions they throw are transported back and
 * rethrown by the outermost wrapper call.
 */

#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "memtkx/dsl/core.hpp"
#include "memtkx/dsl/parser.h"
#include "memtkx/dsl/peg.h"
#include "memtkx/dsl/result.hpp"

namespace MemTKX::dsl {

/* ------------------------------------------------------------------ */
/*  Input and errors                                                  */
/* ------------------------------------------------------------------ */

/** Source text and cursor. */
struct ParsecInput {
  std::string_view source{};
  std::size_t pos{0};

  [[nodiscard]] char peek() const { return pos < source.size() ? source[pos] : '\0'; }
  [[nodiscard]] bool eof() const { return pos >= source.size(); }
  char consume() { return eof() ? '\0' : source[pos++]; }
  [[nodiscard]] std::string_view get_span(std::size_t start) const {
    return source.substr(start, pos - start);
  }

  [[nodiscard]] mtkx_parse_input to_c() const noexcept {
    mtkx_parse_input in = mtkx_parse_input_make(source.data(), source.size());
    in.pos = pos;
    return in;
  }
};

/** Reads a whole file (`Result<contents, message>`). */
inline Result<std::string, std::string> load_parse_input_file(const std::string& path) {
  char* text = nullptr;
  char* message = nullptr;
  std::size_t length = 0;
  const mtkx_status status = mtkx_load_parse_input_file(path.c_str(), &text, &length, &message);
  if (status != MTKX_OK) {
    std::string why = message != nullptr ? std::string(message)
                                         : std::string("mtkx_load_parse_input_file: ") +
                                               mtkx_status_string(status);
    mtkx_free(message);
    return Result<std::string, std::string>::from_err(std::move(why));
  }
  std::string contents(text, length);
  mtkx_free(text);
  return Result<std::string, std::string>::from_ok(std::move(contents));
}

enum class ParseFailureKind { Soft = MTKX_PARSE_SOFT, Committed = MTKX_PARSE_COMMITTED };

struct ParseError {
  std::size_t pos{0};
  ParseFailureKind kind{ParseFailureKind::Soft};
  std::vector<std::string> expected{};
};

namespace detail {

inline ParseError from_c_error(const mtkx_parse_error& e) {
  ParseError out;
  out.pos = e.pos;
  out.kind = static_cast<ParseFailureKind>(e.kind);
  out.expected.reserve(e.expected_count);
  for (std::size_t i = 0; i < e.expected_count; ++i) {
    out.expected.emplace_back(e.expected[i]);
  }
  return out;
}

/** Fills a cleared C error from a C++ one (OOM is recorded in the error). */
inline void to_c_error(const ParseError& e, mtkx_parse_error* out) {
  out->pos = e.pos;
  out->kind = static_cast<mtkx_parse_failure_kind>(e.kind);
  for (const auto& label : e.expected) {
    if (mtkx_parse_error_add_expected(out, label.c_str()) != MTKX_OK) {
      out->kind = MTKX_PARSE_COMMITTED;
      return;
    }
  }
}

struct CErrorGuard {
  mtkx_parse_error error{};
  ~CErrorGuard() { mtkx_parse_error_clear(&error); }
};

}  // namespace detail

/** Merges @p src into @p dst (delegates to mtkx_parse_error_merge). */
inline void merge_error(ParseError& dst, const ParseError& src) {
  detail::CErrorGuard d;
  detail::CErrorGuard s;
  detail::to_c_error(dst, &d.error);
  detail::to_c_error(src, &s.error);
  mtkx_parse_error_merge(&d.error, &s.error);
  if (d.error.out_of_memory || s.error.out_of_memory) {
    throw std::bad_alloc();
  }
  dst = detail::from_c_error(d.error);
}

/** Value or error of one parse step. */
template <typename T>
struct ExpectedResult {
  using value_type = T;

  std::optional<T> value{};
  ParseError error{};

  ExpectedResult() = default;
  ExpectedResult(std::nullopt_t) : value(std::nullopt) {}
  ExpectedResult(const T& v) : value(v) {}
  ExpectedResult(T&& v) : value(std::move(v)) {}

  static ExpectedResult failure(std::size_t pos, ParseFailureKind kind = ParseFailureKind::Soft,
                                std::vector<std::string> expected = {}) {
    ExpectedResult out{};
    out.error = ParseError{pos, kind, std::move(expected)};
    return out;
  }

  explicit operator bool() const { return value.has_value(); }
  T& operator*() { return *value; }
  const T& operator*() const { return *value; }
};

template <typename T>
ExpectedResult<T> fail_expected(const ParsecInput& in, std::string label,
                                ParseFailureKind kind = ParseFailureKind::Soft) {
  return ExpectedResult<T>::failure(in.pos, kind, {std::move(label)});
}

/* ------------------------------------------------------------------ */
/*  PEG value types (shared with peg.hpp)                             */
/* ------------------------------------------------------------------ */

struct PEGRule;

/** A named match stream (C: channel name strings). */
struct PEGChannel {
  std::string name{MTKX_PEG_DEFAULT_CHANNEL};
  bool operator==(const PEGChannel& other) const { return name == other.name; }
  bool operator!=(const PEGChannel& other) const { return name != other.name; }
  [[nodiscard]] bool is_ignore() const noexcept {
    return mtkx_peg_channel_is_ignore(name.c_str());
  }
};

inline const PEGChannel PEGIgnoreChannel{MTKX_PEG_IGNORE_CHANNEL};
inline const PEGChannel PEGDefaultChannel{MTKX_PEG_DEFAULT_CHANNEL};

/** Creates a custom channel; the name must begin with '@'. */
inline PEGChannel new_peg_channel(std::string_view name) {
  std::string n(name);
  if (mtkx_peg_channel_validate(n.c_str()) != MTKX_OK) {
    throw std::invalid_argument("dsl::new_peg_channel: channel names must begin with '@'");
  }
  return PEGChannel{std::move(n)};
}

/** Match record handed to PEG actions; `value` views the parsed source. */
struct PEGMatch {
  std::string_view value{};
  std::size_t begin{0};
  std::size_t end{0};
  PEGRule* rule{nullptr};
  PEGChannel channel{};
  void* user_data{nullptr};
  std::size_t line{1};
  std::size_t column{1};
  bool ignored{false};

  [[nodiscard]] std::size_t length() const noexcept { return end - begin; }
};

/* ------------------------------------------------------------------ */
/*  Value conversion                                                  */
/* ------------------------------------------------------------------ */

namespace detail {

template <typename T>
struct is_pair : std::false_type {};
template <typename A, typename B>
struct is_pair<std::pair<A, B>> : std::true_type {};
template <typename T>
struct is_vector : std::false_type {};
template <typename T, typename A>
struct is_vector<std::vector<T, A>> : std::true_type {};
template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

struct ValueDeleter {
  void operator()(mtkx_parse_value* v) const noexcept { mtkx_parse_value_free(v); }
};
using ValuePtr = std::unique_ptr<mtkx_parse_value, ValueDeleter>;

inline mtkx_parse_value* checked_value(mtkx_parse_value* v) {
  if (v == nullptr) {
    throw std::bad_alloc();
  }
  return v;
}

template <typename T>
void delete_user_value(void* p) noexcept {
  delete static_cast<T*>(p);
}

inline PEGMatch from_c_match(const mtkx_peg_match& m) {
  PEGMatch out;
  out.value = std::string_view(m.value.data, m.value.length);
  out.begin = m.begin;
  out.end = m.end;
  out.rule = m.rule != nullptr ? static_cast<PEGRule*>(m.rule->user_data) : nullptr;
  out.channel = PEGChannel{m.channel != nullptr ? m.channel : MTKX_PEG_DEFAULT_CHANNEL};
  out.user_data = m.user_data;
  out.line = m.line;
  out.column = m.column;
  out.ignored = m.ignored;
  return out;
}

/** Encodes a C++ value as an owned C value (throws std::bad_alloc). */
template <typename T>
ValuePtr to_c_value(T value) {
  if constexpr (std::is_same_v<T, char>) {
    return ValuePtr(checked_value(mtkx_parse_value_char(value)));
  } else if constexpr (std::is_same_v<T, bool>) {
    return ValuePtr(checked_value(mtkx_parse_value_bool(value)));
  } else if constexpr (is_pair<T>::value) {
    ValuePtr first = to_c_value(std::move(value.first));
    ValuePtr second = to_c_value(std::move(value.second));
    return ValuePtr(checked_value(mtkx_parse_value_pair(first.release(), second.release())));
  } else if constexpr (is_vector<T>::value) {
    ValuePtr list(checked_value(mtkx_parse_value_list()));
    for (auto& item : value) {
      ValuePtr c = to_c_value(std::move(item));
      if (mtkx_parse_value_list_push(list.get(), c.release()) != MTKX_OK) {
        throw std::bad_alloc();
      }
    }
    return list;
  } else if constexpr (is_optional<T>::value) {
    if (!value) {
      return ValuePtr(checked_value(mtkx_parse_value_none()));
    }
    ValuePtr inner = to_c_value(std::move(*value));
    return ValuePtr(checked_value(mtkx_parse_value_some(inner.release())));
  } else {
    auto* heap = new T(std::move(value));
    return ValuePtr(checked_value(mtkx_parse_value_user(heap, &delete_user_value<T>)));
  }
}

/** Decodes a C value into T. */
template <typename T>
T from_c_value(const mtkx_parse_value* v) {
  if (v->kind == MTKX_PV_USER) {
    return *static_cast<const T*>(v->as.user.ptr);
  }
  if constexpr (std::is_same_v<T, char>) {
    return v->as.ch;
  } else if constexpr (std::is_same_v<T, bool>) {
    return v->as.boolean;
  } else if constexpr (is_pair<T>::value) {
    return T{from_c_value<typename T::first_type>(v->as.pair.first),
             from_c_value<typename T::second_type>(v->as.pair.second)};
  } else if constexpr (is_vector<T>::value) {
    T out;
    out.reserve(v->as.list.count);
    for (std::size_t i = 0; i < v->as.list.count; ++i) {
      out.push_back(from_c_value<typename T::value_type>(v->as.list.items[i]));
    }
    return out;
  } else if constexpr (is_optional<T>::value) {
    if (v->as.optional == nullptr) {
      return T{};
    }
    return T{from_c_value<typename T::value_type>(v->as.optional)};
  } else if constexpr (std::is_same_v<T, PEGMatch>) {
    return from_c_match(v->as.match);
  } else {
    throw std::logic_error("dsl::Parser: unexpected value representation");
  }
}

/** Converts (and frees) a C result. */
template <typename T>
ExpectedResult<T> from_c_result(mtkx_parse_result* r) {
  struct Free {
    mtkx_parse_result* r;
    ~Free() { mtkx_parse_result_free(r); }
  } guard{r};
  if (!r->ok) {
    if (r->error.out_of_memory && !has_pending_exception()) {
      throw std::bad_alloc();
    }
    ExpectedResult<T> out{};
    out.error = from_c_error(r->error);
    return out;
  }
  return ExpectedResult<T>(from_c_value<T>(r->value));
}

/** Converts a C++ result into a C result (ownership to C). */
template <typename T>
mtkx_parse_result to_c_result(ExpectedResult<T>&& r) {
  if (r.value) {
    return mtkx_parse_ok(to_c_value<T>(std::move(*r.value)).release());
  }
  mtkx_parse_result out = mtkx_parse_fail(r.error.pos,
                                          static_cast<mtkx_parse_failure_kind>(r.error.kind),
                                          nullptr);
  to_c_error(r.error, &out.error);
  return out;
}

/** A committed failure used to unwind the C engine after an exception. */
inline mtkx_parse_result exception_result(std::size_t pos) {
  mtkx_parse_result out = mtkx_parse_fail(pos, MTKX_PARSE_COMMITTED, nullptr);
  out.error.out_of_memory = true; /* stops alternatives and repetition */
  return out;
}

}  // namespace detail

/* ------------------------------------------------------------------ */
/*  Parser                                                            */
/* ------------------------------------------------------------------ */

/** A typed handle to a C parser (copies share the parser). */
template <typename T>
class Parser {
 public:
  using value_type = T;

  Parser() noexcept = default;

  /** Adopts one reference to @p c (throws std::bad_alloc for NULL). */
  explicit Parser(mtkx_parser* c) : c_(c) {
    if (c_ == nullptr) {
      throw std::bad_alloc();
    }
  }

  Parser(const Parser& other) noexcept : c_(mtkx_parser_retain(other.c_)) {}
  Parser(Parser&& other) noexcept : c_(std::exchange(other.c_, nullptr)) {}
  Parser& operator=(Parser other) noexcept {
    std::swap(c_, other.c_);
    return *this;
  }
  ~Parser() { mtkx_parser_release(c_); }

  /** Runs once at the cursor (C++ `p(in)`), advancing `input.pos`. */
  ExpectedResult<T> operator()(ParsecInput& input) const {
    mtkx_parse_input in = input.to_c();
    mtkx_parse_result r = mtkx_parser_parse(c_, &in);
    input.pos = in.pos;
    if (detail::has_pending_exception()) {
      mtkx_parse_result_free(&r);
      detail::rethrow_pending();
    }
    return detail::from_c_result<T>(&r);
  }

  /** Same as operator(). */
  ExpectedResult<T> parse(ParsecInput& input) const { return (*this)(input); }

  /** The C parser (borrowed). */
  [[nodiscard]] mtkx_parser* c_parser() const noexcept { return c_; }
  /** A new reference to the C parser, for handing to C combinators. */
  [[nodiscard]] mtkx_parser* retain_c() const noexcept { return mtkx_parser_retain(c_); }

 private:
  mtkx_parser* c_{nullptr};
};

namespace detail {

template <typename T, typename Fn>
mtkx_parse_result parser_trampoline(mtkx_parse_input* in, void* ctx) {
  if (has_pending_exception()) {
    return exception_result(in->pos);
  }
  try {
    ParsecInput cpp{std::string_view(in->source, in->length), in->pos};
    ExpectedResult<T> r = (*static_cast<Fn*>(ctx))(cpp);
    in->pos = cpp.pos;
    return to_c_result<T>(std::move(r));
  } catch (...) {
    park_current_exception();
    return exception_result(in->pos);
  }
}

template <typename Fn>
void delete_callable(void* p) noexcept {
  delete static_cast<Fn*>(p);
}

template <typename Ret>
struct expected_value;
template <typename T>
struct expected_value<ExpectedResult<T>> {
  using type = T;
};

}  // namespace detail

/** Wraps `ExpectedResult<T>(ParsecInput&)` as a parser. */
template <typename Fn>
auto parser(Fn&& fn) {
  using F = std::decay_t<Fn>;
  using Ret = std::decay_t<decltype(fn(std::declval<ParsecInput&>()))>;
  using T = typename detail::expected_value<Ret>::type;
  auto* heap = new F(std::forward<Fn>(fn));
  return Parser<T>(mtkx_parser_new(&detail::parser_trampoline<T, F>, heap,
                                   &detail::delete_callable<F>));
}

/** Zero or more repetitions (C++ unary `*p`). */
template <typename T>
Parser<std::vector<T>> operator*(const Parser<T>& p) {
  return Parser<std::vector<T>>(mtkx_parser_many(p.retain_c()));
}

/** Sequence producing a pair. */
template <typename A, typename B>
Parser<std::pair<A, B>> operator&(const Parser<A>& a, const Parser<B>& b) {
  return Parser<std::pair<A, B>>(mtkx_parser_seq(a.retain_c(), b.retain_c()));
}

/** Ordered choice. */
template <typename T>
Parser<T> operator|(const Parser<T>& a, const Parser<T>& b) {
  return Parser<T>(mtkx_parser_alt(a.retain_c(), b.retain_c()));
}

template <typename T>
Parser<std::optional<T>> optional(const Parser<T>& p) {
  return Parser<std::optional<T>>(mtkx_parser_optional(p.retain_c()));
}

template <typename T>
Parser<T> try_parse(const Parser<T>& p) {
  return Parser<T>(mtkx_parser_try(p.retain_c()));
}

template <typename T>
Parser<T> labeled(const Parser<T>& p, std::string expected) {
  return Parser<T>(mtkx_parser_labeled(p.retain_c(), expected.c_str()));
}

template <typename T>
struct ParseOutcome {
  std::optional<T> value{};
  ParseError error{};
};

/** Parses the whole of @p source. */
template <typename T>
ParseOutcome<T> run_parser(const Parser<T>& p, std::string_view source) {
  mtkx_parse_result r = mtkx_run_parser(p.c_parser(), source.data(), source.size());
  if (detail::has_pending_exception()) {
    mtkx_parse_result_free(&r);
    detail::rethrow_pending();
  }
  ExpectedResult<T> e = detail::from_c_result<T>(&r);
  if (!e) {
    return {std::nullopt, std::move(e.error)};
  }
  return {std::move(e.value), {}};
}

inline Parser<char> ch(char c) { return Parser<char>(mtkx_parser_ch(c)); }

inline Parser<char> satisfy(std::function<bool(char)> pred, std::string label) {
  return labeled(parser([pred = std::move(pred), label](ParsecInput& in) -> ExpectedResult<char> {
                   if (!in.eof() && pred(in.peek())) {
                     return in.consume();
                   }
                   return fail_expected<char>(in, label);
                 }),
                 label);
}

/* ------------------------------------------------------------------ */
/*  Semantic actions and productions                                  */
/* ------------------------------------------------------------------ */

struct ParserStage {
  std::size_t begin{};
  std::size_t end{};
  std::string_view span{};
};

template <typename T, typename ActionFn>
auto parse_with_action(const Parser<T>& p, ParsecInput& input,
                       const std::unordered_map<std::string, std::string>& semantics,
                       ActionFn&& action)
    -> std::optional<decltype(action(semantics, ParserStage{}))> {
  const std::size_t begin = input.pos;
  auto r = p(input);
  if (!r) {
    return std::nullopt;
  }
  ParserStage stage{begin, input.pos, input.get_span(begin)};
  return action(semantics, stage);
}

/** A production handler with a runtime id. */
template <typename Fn>
struct NamedProduction {
  std::string_view id;
  Fn fn;
};

template <typename Fn>
auto production(std::string_view id, Fn&& fn) {
  return NamedProduction<std::decay_t<Fn>>{id, std::forward<Fn>(fn)};
}

#if MEMTKX_CPP20
template <FixedString Id, typename Fn>
struct ProductionHandler {
  static constexpr std::string_view id = Id.view();
  Fn fn;
};

template <FixedString Id, typename Fn>
auto production(Fn&& fn) {
  return ProductionHandler<Id, std::decay_t<Fn>>{std::forward<Fn>(fn)};
}
#endif

}  // namespace MemTKX::dsl
