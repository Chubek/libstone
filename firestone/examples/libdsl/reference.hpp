#pragma once

#include "LibDSL.hpp"

namespace dsl::synth::reference
{
namespace detail
{
using Int = std::int64_t;
inline constexpr auto min_int = std::numeric_limits<Int>::min ();
inline constexpr auto max_int = std::numeric_limits<Int>::max ();

inline bool
decimal (std::string_view text)
{
  if (text.empty ())
    return false;
  if (text.front () == '-')
    text.remove_prefix (1);
  return !text.empty () && std::all_of (text.begin (), text.end (), [] (char c) {
    return c >= '0' && c <= '9';
  });
}

inline Int
integer (std::string_view text)
{
  if (!decimal (text))
    throw std::invalid_argument ("expected decimal integer");
  // Accumulate negatively, so both signed extrema are representable.
  const bool negative = text.front () == '-';
  if (negative)
    text.remove_prefix (1);
  const Int limit = negative ? min_int : -max_int;
  Int n = 0;
  for (char c : text)
    {
      const Int digit = c - '0';
      if (n < (limit + digit) / 10)
        throw std::overflow_error ("integer literal overflow");
      n = n * 10 - digit;
    }
  return negative ? n : -n;
}

inline Int
add (Int a, Int b)
{
  if ((b > 0 && a > max_int - b) || (b < 0 && a < min_int - b))
    throw std::overflow_error ("addition overflow");
  return a + b;
}

inline Int
subtract (Int a, Int b)
{
  if ((b > 0 && a < min_int + b) || (b < 0 && a > max_int + b))
    throw std::overflow_error ("subtraction overflow");
  return a - b;
}

inline Int
multiply (Int a, Int b)
{
  if ((a > 0 && ((b > 0 && a > max_int / b) || (b < 0 && b < min_int / a)))
      || (a < 0 && ((b > 0 && a < min_int / b) || (b < 0 && a < max_int / b))))
    throw std::overflow_error ("multiplication overflow");
  return a * b;
}

inline Int
divide (Int a, Int b)
{
  if (b == 0)
    throw std::domain_error ("division by zero");
  if (a == min_int && b == -1)
    throw std::overflow_error ("division overflow");
  return a / b;
}

inline std::string
name (const SExpr &syntax)
{
  if (!syntax.is_atom || syntax.quoted || decimal (syntax.text)
      || syntax.text == "true" || syntax.text == "false")
    throw std::invalid_argument ("expected binding name");
  return syntax.text;
}

template <class Lang, class Unpack, class Fn>
void
binary (Lang &lang, std::string name, Unpack unpack, Fn function)
{
  lang.on_expression (std::move (name), [=] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 2);
    auto left = builder.expression (syntax.items[1]);
    auto right = builder.expression (syntax.items[2]);
    return builder.expression ([=] (auto &frame) {
      const auto a = unpack (frame.evaluate (left));
      const auto b = unpack (frame.evaluate (right));
      return function (a, b);
    });
  });
}

template <class Lang>
void
arithmetic_forms (Lang &lang)
{
  const auto identity = [] (Int n) { return n; };
  binary (lang, "+", identity, add);
  binary (lang, "-", identity, subtract);
  binary (lang, "*", identity, multiply);
  binary (lang, "/", identity, divide);
  binary (lang, "=", identity, [] (Int a, Int b) { return Int (a == b); });
  binary (lang, "<", identity, [] (Int a, Int b) { return Int (a < b); });
}
} // namespace detail

struct ArithmeticState {};
using Arithmetic = Executable<ArithmeticState>;

inline Arithmetic
arithmetic ()
{
  Arithmetic lang;
  detail::arithmetic_forms (lang);
  lang.on_atom ([] (auto &builder, const SExpr &syntax) {
    if (syntax.quoted)
      throw std::invalid_argument ("arithmetic does not accept strings");
    const auto n = detail::integer (syntax.text);
    return builder.expression ([n] (auto &) { return n; });
  });
  return lang;
}

struct HoareState
{
  std::unordered_map<std::string, detail::Int> variables;
};
using Hoare = Executable<HoareState>;

inline Hoare
hoare ()
{
  Hoare lang;
  detail::arithmetic_forms (lang);
  lang.on_atom ([] (auto &builder, const SExpr &syntax) {
    if (!syntax.quoted && detail::decimal (syntax.text))
      {
        const auto n = detail::integer (syntax.text);
        return builder.expression ([n] (auto &) { return n; });
      }
    return builder.expression ([key = detail::name (syntax)] (auto &frame) {
      const auto &variables = frame.state ().variables;
      const auto pos = variables.find (key);
      if (pos == variables.end ())
        throw std::runtime_error ("unbound imperative variable: " + key);
      return pos->second;
    });
  });
  lang.on_statement ("set", [] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 2);
    auto rhs = builder.expression (syntax.items[2]);
    return builder.statement ([key = detail::name (syntax.items[1]), rhs] (auto &frame) {
      const auto value = frame.evaluate (rhs);
      frame.state ().variables.insert_or_assign (key, value);
    });
  });
  lang.on_statement ("begin", [] (auto &builder, const SExpr &syntax) {
    std::vector<Hoare::Statement> statements;
    for (std::size_t i = 1; i < syntax.items.size (); ++i)
      statements.push_back (builder.statement (syntax.items[i]));
    return builder.statement ([statements = std::move (statements)] (auto &frame) {
      for (const auto &statement : statements)
        frame.execute (statement);
    });
  });
  lang.on_statement ("if", [] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 3);
    auto condition = builder.expression (syntax.items[1]);
    auto yes = builder.statement (syntax.items[2]);
    auto no = builder.statement (syntax.items[3]);
    return builder.statement ([=] (auto &frame) {
      frame.execute (frame.evaluate (condition) ? yes : no);
    });
  });
  lang.on_statement ("while", [] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 3);
    auto condition = builder.expression (syntax.items[1]);
    auto invariant = builder.expression (syntax.items[2]);
    auto body = builder.statement (syntax.items[3]);
    return builder.statement ([=] (auto &frame) {
      for (;;)
        {
          if (!frame.evaluate (invariant))
            throw std::runtime_error ("loop invariant violated");
          if (!frame.evaluate (condition))
            break;
          frame.execute (body);
        }
    });
  });
  lang.on_statement ("triple", [] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 3);
    auto precondition = builder.expression (syntax.items[1]);
    auto body = builder.statement (syntax.items[2]);
    auto postcondition = builder.expression (syntax.items[3]);
    return builder.statement ([=] (auto &frame) {
      if (!frame.evaluate (precondition))
        throw std::runtime_error ("precondition violated");
      frame.execute (body);
      if (!frame.evaluate (postcondition))
        throw std::runtime_error ("postcondition violated");
    });
  });
  return lang;
}

struct LispClosure;
using LispValue = std::variant<detail::Int, bool, std::string,
                               std::shared_ptr<const LispClosure>>;
struct LispScope
{
  std::shared_ptr<const LispScope> parent;
  std::unordered_map<std::string, LispValue> bindings;
  std::size_t depth = 1;
};
struct LispState
{
  std::shared_ptr<const LispScope> scope;
  // A host parameter is read through the custom metaprocedure below; ordinary
  // Lisp bindings and closures still use lexical scoping.
  detail::Int context_value = 0;
};
using Lisp = Executable<LispState, LispValue>;
struct LispClosure
{
  std::vector<std::string> parameters;
  Lisp::Expression body;
  std::shared_ptr<const LispScope> scope;
};

namespace detail
{
inline std::shared_ptr<const LispScope>
scope (std::shared_ptr<const LispScope> parent,
       std::unordered_map<std::string, LispValue> bindings)
{
  if (parent && parent->depth >= 128)
    throw std::runtime_error ("lexical scope depth limit exceeded");
  const auto depth = parent ? parent->depth + 1 : 1;
  return std::make_shared<const LispScope> (
      LispScope{ std::move (parent), std::move (bindings), depth });
}

struct RestoreScope
{
  LispState &state;
  std::shared_ptr<const LispScope> saved;
  RestoreScope (LispState &state, std::shared_ptr<const LispScope> current)
    : state (state), saved (std::exchange (state.scope, std::move (current)))
  {
  }
  ~RestoreScope () { state.scope = std::move (saved); }
};

inline Int
lisp_int (const LispValue &value)
{
  if (const auto *n = std::get_if<Int> (&value))
    return *n;
  throw std::runtime_error ("expected Lisp integer");
}
} // namespace detail

inline Lisp
lisp ()
{
  Lisp lang;
  detail::binary (lang, "+", detail::lisp_int, detail::add);
  detail::binary (lang, "-", detail::lisp_int, detail::subtract);
  detail::binary (lang, "*", detail::lisp_int, detail::multiply);
  detail::binary (lang, "/", detail::lisp_int, detail::divide);
  detail::binary (lang, "=", detail::lisp_int,
                  [] (detail::Int a, detail::Int b) { return a == b; });
  using Procedures = MetaProcedures<LispState, LispValue>;
  auto procedures = std::make_shared<Procedures> ();
  procedures->define_text ("(metaproc context-value analyze custom ())",
      Procedures::Analyzer{ [] (const Procedures::Context &context) {
        return context.resolve ("host-context");
      } }, {}, [] (const LispState &state, const Scope<LispValue> &definition,
                   const Scope<LispValue> &) {
        // Explicitly import the current host context into the definition
        // environment; never read the caller's lexical binding named the same.
        return definition.extend ({ { "host-context", LispValue (state.context_value), {} } });
      });
  lang.on_expression ("context-value", [procedures] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 0);
    return builder.expression ([procedures] (auto &frame) {
      return procedures->analyze (frame.state (), {}, { "context-value", {} });
    });
  });
  lang.on_atom ([] (auto &builder, const SExpr &syntax) {
    if (syntax.quoted)
      return builder.expression ([text = syntax.text] (auto &) { return LispValue (text); });
    if (detail::decimal (syntax.text))
      return builder.expression ([n = detail::integer (syntax.text)] (auto &) {
        return LispValue (n);
      });
    if (syntax.text == "true" || syntax.text == "false")
      return builder.expression ([value = syntax.text == "true"] (auto &) {
        return LispValue (value);
      });
    return builder.expression ([key = detail::name (syntax)] (auto &frame) {
      for (auto scope = frame.state ().scope; scope; scope = scope->parent)
        {
          frame.tick ();
          if (auto pos = scope->bindings.find (key); pos != scope->bindings.end ())
            return pos->second;
        }
      throw std::runtime_error ("unbound Lisp variable: " + key);
    });
  });
  lang.on_expression ("if", [] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 3);
    auto condition = builder.expression (syntax.items[1]);
    auto yes = builder.expression (syntax.items[2]);
    auto no = builder.expression (syntax.items[3]);
    return builder.expression ([=] (auto &frame) {
      const auto value = frame.evaluate (condition);
      if (const auto *test = std::get_if<bool> (&value))
        return frame.evaluate (*test ? yes : no);
      throw std::runtime_error ("expected Lisp boolean");
    });
  });
  lang.on_expression ("let", [] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 3);
    auto value = builder.expression (syntax.items[2]);
    auto body = builder.expression (syntax.items[3]);
    return builder.expression ([key = detail::name (syntax.items[1]), value, body] (auto &frame) {
      auto bound = frame.evaluate (value);
      detail::RestoreScope restore (frame.state (),
          detail::scope (frame.state ().scope, { { key, std::move (bound) } }));
      return frame.evaluate (body);
    });
  });
  lang.on_expression ("lambda", [] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 2);
    if (syntax.items[1].is_atom)
      throw std::invalid_argument ("lambda requires a parameter list");
    std::vector<std::string> parameters;
    std::unordered_set<std::string> distinct;
    for (const auto &parameter : syntax.items[1].items)
      {
        auto key = detail::name (parameter);
        if (!distinct.insert (key).second)
          throw std::invalid_argument ("duplicate Lisp parameter");
        parameters.push_back (std::move (key));
      }
    auto body = builder.expression (syntax.items[2]);
    return builder.expression ([parameters = std::move (parameters), body] (auto &frame) {
      return LispValue (std::make_shared<const LispClosure> (
          LispClosure{ parameters, body, frame.state ().scope }));
    });
  });
  lang.on_application ([] (auto &builder, const SExpr &syntax) {
    if (syntax.items.empty ())
      throw std::invalid_argument ("empty Lisp application");
    auto callable = builder.expression (syntax.items.front ());
    std::vector<Lisp::Expression> arguments;
    for (std::size_t i = 1; i < syntax.items.size (); ++i)
      arguments.push_back (builder.expression (syntax.items[i]));
    return builder.expression ([callable, arguments = std::move (arguments)] (auto &frame) {
      const auto value = frame.evaluate (callable);
      const auto *closure = std::get_if<std::shared_ptr<const LispClosure>> (&value);
      if (!closure || !*closure)
        throw std::runtime_error ("application requires a Lisp closure");
      if ((*closure)->parameters.size () != arguments.size ())
        throw std::runtime_error ("wrong Lisp application arity");
      std::unordered_map<std::string, LispValue> bindings;
      for (std::size_t i = 0; i < arguments.size (); ++i)
        bindings.emplace ((*closure)->parameters[i], frame.evaluate (arguments[i]));
      detail::RestoreScope restore (frame.state (),
          detail::scope ((*closure)->scope, std::move (bindings)));
      return frame.evaluate ((*closure)->body);
    });
  });
  lang.on_statement ("define", [] (auto &builder, const SExpr &syntax) {
    builder.require_arity (syntax, 2);
    auto value = builder.expression (syntax.items[2]);
    return builder.statement ([key = detail::name (syntax.items[1]), value] (auto &frame) {
      auto bound = frame.evaluate (value);
      frame.state ().scope = detail::scope (frame.state ().scope,
                                           { { key, std::move (bound) } });
    });
  });
  return lang;
}
} // namespace dsl::synth::reference
