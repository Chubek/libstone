#pragma once

// Nerotypical demo language — constraint generation for compilers and
// interpreters.
//
// The AST below is the "client IR" half of the embedding contract: a
// compiler front-end lowers its own syntax to these nodes (or generates
// constraints directly against the Mode A / Mode B APIs) and Nerotypical
// produces the typing constraints and solves them.  Two checkers consume
// the same AST:
//
//   SetTypeChecker     — set-typing mode.  Types are unions over the
//                        registered atom universe (int, float, bool,
//                        string, unit by default).  A conservative static
//                        checker: every branch is checked, arithmetic is
//                        numeric-only, `if` joins branches by union.  No
//                        function types: lambdas are rejected with a
//                        diagnostic pointing at the structural mode.
//
//   StructuralChecker  — unification mode.  Monomorphic simple types
//                        (int/float/bool/string/unit, fun/list/pair
//                        constructors); lambdas and applications
//                        typecheck; arithmetic and comparisons are
//                        int-only.
//
// Both checkers are monomorphic (a `let` binding is shared across uses)
// and both attach the originating source span to every generated
// constraint, so errors point back at the source.  Checker instances are
// reusable: each `check` call builds a fresh constraint system / type
// graph; embedder-registered atoms/constructors persist across calls.

#include "Analysis.hpp"
#include "SetTyping.hpp"
#include "StructuralTyping.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace moonstone::nerotypical::ast {

enum class BinOp : std::uint8_t
{
  Add,
  Sub,
  Mul,
  Div,
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,
  And,
  Or
};

enum class UnOp : std::uint8_t
{
  Neg,
  Not
};

struct Expr
{
  enum class Kind : std::uint8_t
  {
    IntLiteral,
    FloatLiteral,
    BoolLiteral,
    StringLiteral,
    UnitLiteral,
    Variable,
    Let,
    If,
    Binary,
    Unary,
    Call,
    Lambda,
    Apply
  };

  Kind kind = Kind::UnitLiteral;
  std::string name;  // Variable / Let binder / Call callee / Lambda parameter
  std::int64_t int_value = 0;  // literal value, or the BinOp/UnOp tag
  double float_value = 0.0;
  bool bool_value = false;
  SourceSpan span{};
  // Let: [value, body]; If: [cond, then, else]; Binary: [lhs, rhs];
  // Unary: [operand]; Call: arguments; Lambda: [body]; Apply: [fn, arg].
  std::vector<Expr> children;
};

struct Function
{
  std::string name;
  std::vector<std::string> params;
  Expr body;
  SourceSpan span;
};

struct Program
{
  std::vector<Function> functions;
  Expr body;
};

inline Expr int_literal (std::int64_t value, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::IntLiteral;
  e.int_value = value;
  e.span = span;
  return e;
}

inline Expr float_literal (double value, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::FloatLiteral;
  e.float_value = value;
  e.span = span;
  return e;
}

inline Expr bool_literal (bool value, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::BoolLiteral;
  e.bool_value = value;
  e.span = span;
  return e;
}

inline Expr string_literal (SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::StringLiteral;
  e.span = span;
  return e;
}

inline Expr unit_literal (SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::UnitLiteral;
  e.span = span;
  return e;
}

inline Expr variable (std::string name, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::Variable;
  e.name = std::move (name);
  e.span = span;
  return e;
}

inline Expr let (std::string binder, Expr value, Expr body, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::Let;
  e.name = std::move (binder);
  e.span = span;
  e.children.push_back (std::move (value));
  e.children.push_back (std::move (body));
  return e;
}

inline Expr if_then_else (Expr condition, Expr then_branch, Expr else_branch, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::If;
  e.span = span;
  e.children.push_back (std::move (condition));
  e.children.push_back (std::move (then_branch));
  e.children.push_back (std::move (else_branch));
  return e;
}

inline Expr binary (BinOp op, Expr lhs, Expr rhs, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::Binary;
  e.int_value = static_cast<std::int64_t> (op);
  e.span = span;
  e.children.push_back (std::move (lhs));
  e.children.push_back (std::move (rhs));
  return e;
}

inline Expr unary (UnOp op, Expr operand, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::Unary;
  e.int_value = static_cast<std::int64_t> (op);
  e.span = span;
  e.children.push_back (std::move (operand));
  return e;
}

inline Expr call (std::string callee, std::vector<Expr> args, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::Call;
  e.name = std::move (callee);
  e.span = span;
  e.children = std::move (args);
  return e;
}

inline Expr lambda (std::string parameter, Expr body, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::Lambda;
  e.name = std::move (parameter);
  e.span = span;
  e.children.push_back (std::move (body));
  return e;
}

inline Expr apply (Expr fn, Expr argument, SourceSpan span = {})
{
  Expr e;
  e.kind = Expr::Kind::Apply;
  e.span = span;
  e.children.push_back (std::move (fn));
  e.children.push_back (std::move (argument));
  return e;
}

inline BinOp binary_op (const Expr &e) { return static_cast<BinOp> (e.int_value); }
inline UnOp unary_op (const Expr &e) { return static_cast<UnOp> (e.int_value); }

} // namespace moonstone::nerotypical::ast

namespace moonstone::nerotypical::language {

/// Set-typing front-end over the demo AST.
class SetTypeChecker
{
public:
  SetTypeChecker ()
  {
    int_ = universe_.intern ("int");
    float_ = universe_.intern ("float");
    bool_ = universe_.intern ("bool");
    string_ = universe_.intern ("string");
    unit_ = universe_.intern ("unit");
    numeric_ = { int_, float_ };
  }

  /// Registers an extra atom for embedder-specific value shapes.
  settype::AtomId atom (const std::string &name) { return universe_.intern (name); }
  std::optional<settype::AtomId> atom_id (const std::string &name) const
  {
    return universe_.find (name);
  }

  /// Type-checks the program.  The returned witness typing is indexed by
  /// type variable id; `binding_type` maps names to those ids.
  settype::SetTypingResult check (const ast::Program &program,
                                  const AnalysisOptions &options = {})
  {
    settype::ConstraintSystem system (universe_);
    bindings_.clear ();
    program_result_ = no_variable;

    std::map<std::string, Signature> signatures;
    Generation state;
    for (const ast::Function &fn : program.functions)
      {
        if (signatures.count (fn.name) != 0)
          return generation_error ("duplicate function '" + fn.name + "'", fn.span);
        Signature &sig = signatures[fn.name];
        for (std::size_t i = 0; i < fn.params.size (); ++i)
          {
            const settype::TypeVarId param = system.fresh_variable (fn.params[i]);
            sig.params.push_back (param);
            bindings_[fn.params[i]] = param;
          }
        sig.result = system.fresh_variable ("result of " + fn.name);
      }

    for (const ast::Function &fn : program.functions)
      {
        std::map<std::string, settype::TypeVarId> env;
        const Signature &sig = signatures.at (fn.name);
        for (std::size_t i = 0; i < fn.params.size (); ++i)
          env[fn.params[i]] = sig.params[i];
        const std::optional<settype::TypeVarId> body =
            generate (system, env, signatures, fn.body, state);
        if (!body)
          return generation_error (*state.error, state.span);
        settype::TypeConstraint c;
        c.kind = settype::TypeConstraint::Kind::Equal;
        c.a = settype::variable_term (sig.result);
        c.b = settype::variable_term (*body);
        c.span = fn.span;
        c.note = "body of function '" + fn.name + "'";
        if (auto error = system.add (std::move (c)))
          return generation_error (*error, fn.span);
      }

    {
      const std::optional<settype::TypeVarId> body =
          generate (system, {}, signatures, program.body, state);
      if (!body)
        return generation_error (*state.error, state.span);
      program_result_ = *body;
    }

    settype::SetTypingResult result = system.solve (options);
    system_ = std::move (system);
    return result;
  }

  /// Type variable of a named let binding / function parameter from the
  /// last `check` call; `no_variable` when absent.
  settype::TypeVarId binding_type (const std::string &name) const
  {
    if (auto it = bindings_.find (name); it != bindings_.end ())
      return it->second;
    return no_variable;
  }

  settype::TypeVarId program_result () const { return program_result_; }
  const settype::ConstraintSystem &system () const { return system_; }

  static constexpr settype::TypeVarId no_variable = static_cast<settype::TypeVarId> (-1);

private:
  struct Signature
  {
    std::vector<settype::TypeVarId> params;
    settype::TypeVarId result = no_variable;
  };

  struct Generation
  {
    std::optional<std::string> error;
    SourceSpan span{};
  };

  settype::SetTypingResult generation_error (const std::string &message, SourceSpan span)
  {
    settype::SetTypingResult result;
    push_diagnostic (result.base, "nerotypical.generation", Severity::Error, message, span);
    return result;
  }

  bool add_checked (settype::ConstraintSystem &system, Generation &state,
                    settype::TypeConstraint constraint)
  {
    if (auto error = system.add (std::move (constraint)))
      {
        state.error = *error;
        state.span = constraint.span;
        return false;
      }
    return true;
  }

  std::optional<settype::TypeVarId> generate (
      settype::ConstraintSystem &system, const std::map<std::string, settype::TypeVarId> &env,
      const std::map<std::string, Signature> &signatures, const ast::Expr &expr, Generation &state)
  {
    if (state.error)
      return std::nullopt;

    auto literal = [&](settype::AtomId atom, const char *note) -> std::optional<settype::TypeVarId> {
      const settype::TypeVarId t = system.fresh_variable ();
      settype::TypeConstraint c;
      c.kind = settype::TypeConstraint::Kind::Equal;
      c.a = settype::variable_term (t);
      c.b = settype::constant_term ({ atom });
      c.span = expr.span;
      c.note = note;
      if (!add_checked (system, state, std::move (c)))
        return std::nullopt;
      return t;
    };

    switch (expr.kind)
      {
      case ast::Expr::Kind::IntLiteral:
        return literal (int_, "integer literal");
      case ast::Expr::Kind::FloatLiteral:
        return literal (float_, "floating-point literal");
      case ast::Expr::Kind::BoolLiteral:
        return literal (bool_, "boolean literal");
      case ast::Expr::Kind::StringLiteral:
        return literal (string_, "string literal");
      case ast::Expr::Kind::UnitLiteral:
        return literal (unit_, "unit literal");
      case ast::Expr::Kind::Variable:
        {
          auto it = env.find (expr.name);
          if (it == env.end ())
            {
              state.error = "unbound variable '" + expr.name + "'";
              state.span = expr.span;
              return std::nullopt;
            }
          return it->second;
        }
      case ast::Expr::Kind::Let:
        {
          const auto value = generate (system, env, signatures, expr.children.at (0), state);
          if (!value)
            return std::nullopt;
          std::map<std::string, settype::TypeVarId> inner = env;
          inner[expr.name] = *value;
          bindings_[expr.name] = *value;
          return generate (system, inner, signatures, expr.children.at (1), state);
        }
      case ast::Expr::Kind::If:
        {
          const auto condition = generate (system, env, signatures, expr.children.at (0), state);
          if (!condition)
            return std::nullopt;
          settype::TypeConstraint c;
          c.kind = settype::TypeConstraint::Kind::Equal;
          c.a = settype::variable_term (*condition);
          c.b = settype::constant_term ({ bool_ });
          c.span = expr.children.at (0).span;
          c.note = "if condition must be bool";
          if (!add_checked (system, state, std::move (c)))
            return std::nullopt;
          const auto then_branch = generate (system, env, signatures, expr.children.at (1), state);
          if (!then_branch)
            return std::nullopt;
          const auto else_branch = generate (system, env, signatures, expr.children.at (2), state);
          if (!else_branch)
            return std::nullopt;
          const settype::TypeVarId result = system.fresh_variable ("if result");
          settype::TypeConstraint u;
          u.kind = settype::TypeConstraint::Kind::Union;
          u.result = settype::variable_term (result);
          u.a = settype::variable_term (*then_branch);
          u.b = settype::variable_term (*else_branch);
          u.span = expr.span;
          u.note = "if result: union of both branches";
          if (!add_checked (system, state, std::move (u)))
            return std::nullopt;
          return result;
        }
      case ast::Expr::Kind::Binary:
        {
          const ast::BinOp op = ast::binary_op (expr);
          const auto lhs = generate (system, env, signatures, expr.children.at (0), state);
          if (!lhs)
            return std::nullopt;
          const auto rhs = generate (system, env, signatures, expr.children.at (1), state);
          if (!rhs)
            return std::nullopt;
          const bool arithmetic = op == ast::BinOp::Add || op == ast::BinOp::Sub ||
                                  op == ast::BinOp::Mul || op == ast::BinOp::Div;
          const bool comparison = op == ast::BinOp::Lt || op == ast::BinOp::Le ||
                                  op == ast::BinOp::Gt || op == ast::BinOp::Ge;
          const bool logical = op == ast::BinOp::And || op == ast::BinOp::Or;
          const settype::TypeVarId result = system.fresh_variable ();
          if (arithmetic)
            {
              for (const settype::TypeVarId operand : { *lhs, *rhs })
                {
                  settype::TypeConstraint c;
                  c.kind = settype::TypeConstraint::Kind::Subtype;
                  c.a = settype::variable_term (operand);
                  c.b = settype::constant_term (numeric_);
                  c.span = expr.span;
                  c.note = "arithmetic operand must be numeric";
                  if (!add_checked (system, state, std::move (c)))
                    return std::nullopt;
                }
              settype::TypeConstraint u;
              u.kind = settype::TypeConstraint::Kind::Union;
              u.result = settype::variable_term (result);
              u.a = settype::variable_term (*lhs);
              u.b = settype::variable_term (*rhs);
              u.span = expr.span;
              u.note = "arithmetic result: union of operand types";
              if (!add_checked (system, state, std::move (u)))
                return std::nullopt;
              return result;
            }
          if (comparison)
            {
              for (const settype::TypeVarId operand : { *lhs, *rhs })
                {
                  settype::TypeConstraint c;
                  c.kind = settype::TypeConstraint::Kind::Subtype;
                  c.a = settype::variable_term (operand);
                  c.b = settype::constant_term (numeric_);
                  c.span = expr.span;
                  c.note = "comparison operand must be numeric";
                  if (!add_checked (system, state, std::move (c)))
                    return std::nullopt;
                }
            }
          if (logical)
            {
              for (const settype::TypeVarId operand : { *lhs, *rhs })
                {
                  settype::TypeConstraint c;
                  c.kind = settype::TypeConstraint::Kind::Equal;
                  c.a = settype::variable_term (operand);
                  c.b = settype::constant_term ({ bool_ });
                  c.span = expr.span;
                  c.note = "logical operand must be bool";
                  if (!add_checked (system, state, std::move (c)))
                    return std::nullopt;
                }
            }
          settype::TypeConstraint c;
          c.kind = settype::TypeConstraint::Kind::Equal;
          c.a = settype::variable_term (result);
          c.b = settype::constant_term ({ bool_ });
          c.span = expr.span;
          c.note = "operator result is bool";
          if (!add_checked (system, state, std::move (c)))
            return std::nullopt;
          return result;
        }
      case ast::Expr::Kind::Unary:
        {
          const auto operand = generate (system, env, signatures, expr.children.at (0), state);
          if (!operand)
            return std::nullopt;
          const settype::TypeVarId result = system.fresh_variable ();
          if (ast::unary_op (expr) == ast::UnOp::Neg)
            {
              settype::TypeConstraint c;
              c.kind = settype::TypeConstraint::Kind::Subtype;
              c.a = settype::variable_term (*operand);
              c.b = settype::constant_term (numeric_);
              c.span = expr.span;
              c.note = "negation operand must be numeric";
              if (!add_checked (system, state, std::move (c)))
                return std::nullopt;
              settype::TypeConstraint u;
              u.kind = settype::TypeConstraint::Kind::Union;
              u.result = settype::variable_term (result);
              u.a = settype::variable_term (*operand);
              u.b = settype::variable_term (*operand);
              u.span = expr.span;
              u.note = "negation result";
              if (!add_checked (system, state, std::move (u)))
                return std::nullopt;
              return result;
            }
          settype::TypeConstraint c;
          c.kind = settype::TypeConstraint::Kind::Equal;
          c.a = settype::variable_term (*operand);
          c.b = settype::constant_term ({ bool_ });
          c.span = expr.span;
          c.note = "not operand must be bool";
          if (!add_checked (system, state, std::move (c)))
            return std::nullopt;
          settype::TypeConstraint r;
          r.kind = settype::TypeConstraint::Kind::Equal;
          r.a = settype::variable_term (result);
          r.b = settype::constant_term ({ bool_ });
          r.span = expr.span;
          r.note = "not result is bool";
          if (!add_checked (system, state, std::move (r)))
            return std::nullopt;
          return result;
        }
      case ast::Expr::Kind::Call:
        {
          auto it = signatures.find (expr.name);
          if (it == signatures.end ())
            {
              state.error = "call to undeclared function '" + expr.name + "'";
              state.span = expr.span;
              return std::nullopt;
            }
          const Signature &sig = it->second;
          if (expr.children.size () != sig.params.size ())
            {
              state.error = "call to '" + expr.name + "' with " +
                            std::to_string (expr.children.size ()) + " arguments, expected " +
                            std::to_string (sig.params.size ());
              state.span = expr.span;
              return std::nullopt;
            }
          for (std::size_t i = 0; i < expr.children.size (); ++i)
            {
              const auto argument =
                  generate (system, env, signatures, expr.children.at (i), state);
              if (!argument)
                return std::nullopt;
              settype::TypeConstraint c;
              c.kind = settype::TypeConstraint::Kind::Equal;
              c.a = settype::variable_term (*argument);
              c.b = settype::variable_term (sig.params[i]);
              c.span = expr.children.at (i).span;
              c.note = "argument " + std::to_string (i + 1) + " of '" + expr.name + "'";
              if (!add_checked (system, state, std::move (c)))
                return std::nullopt;
            }
          return sig.result;
        }
      case ast::Expr::Kind::Lambda:
        state.error = "set typing has no function types; use the structural mode for lambdas";
        state.span = expr.span;
        return std::nullopt;
      case ast::Expr::Kind::Apply:
        state.error = "set typing has no function types; use the structural mode for application";
        state.span = expr.span;
        return std::nullopt;
      }
    state.error = "unknown expression kind";
    state.span = expr.span;
    return std::nullopt;
  }

  settype::TypeUniverse universe_{};
  settype::AtomId int_ = 0, float_ = 0, bool_ = 0, string_ = 0, unit_ = 0;
  settype::AtomSet numeric_{};
  std::map<std::string, settype::TypeVarId> bindings_{};
  settype::TypeVarId program_result_ = no_variable;
  settype::ConstraintSystem system_{};
};

/// Unification front-end over the demo AST.
class StructuralChecker
{
public:
  StructuralChecker ()
  {
    add_constructor ("int", 0);
    add_constructor ("float", 0);
    add_constructor ("bool", 0);
    add_constructor ("string", 0);
    add_constructor ("unit", 0);
    add_constructor ("fun", 2);
    add_constructor ("list", 1);
    add_constructor ("pair", 2);
  }

  /// Registers a constructor for embedder-specific type structure.
  /// Returns an error message on an empty or duplicate name.
  std::optional<std::string> add_constructor (std::string name, int arity)
  {
    if (name.empty ())
      return "constructor name must not be empty";
    for (const auto &entry : registry_)
      if (entry.first == name)
        return "constructor " + name + " is already registered";
    if (arity < 0)
      return "constructor " + name + " has a negative arity";
    registry_.emplace_back (std::move (name), arity);
    return std::nullopt;
  }

  /// Constructor ids are registry indices and stable across `check` calls.
  std::optional<structural::ConstructorId> constructor_id (const std::string &name) const
  {
    for (std::size_t i = 0; i < registry_.size (); ++i)
      if (registry_[i].first == name)
        return static_cast<structural::ConstructorId> (i);
    return std::nullopt;
  }

  structural::StructuralResult check (const ast::Program &program,
                                      const AnalysisOptions &options = {})
  {
    structural::TypeGraph graph;
    for (const auto &entry : registry_)
      if (auto error = graph.add_constructor (entry.first, entry.second))
        return generation_error (*error, SourceSpan{});

    bindings_.clear ();
    program_result_ = no_term;

    std::map<std::string, Signature> signatures;
    Generation state;
    for (const ast::Function &fn : program.functions)
      {
        if (signatures.count (fn.name) != 0)
          return generation_error ("duplicate function '" + fn.name + "'", fn.span);
        Signature &sig = signatures[fn.name];
        for (std::size_t i = 0; i < fn.params.size (); ++i)
          {
            const structural::TermId param = graph.fresh_variable (fn.params[i]);
            sig.params.push_back (param);
            bindings_[fn.params[i]] = param;
          }
        sig.result = graph.fresh_variable ("result of " + fn.name);
      }

    for (const ast::Function &fn : program.functions)
      {
        std::map<std::string, structural::TermId> env;
        const Signature &sig = signatures.at (fn.name);
        for (std::size_t i = 0; i < fn.params.size (); ++i)
          env[fn.params[i]] = sig.params[i];
        const std::optional<structural::TermId> body =
            generate (graph, env, signatures, fn.body, state);
        if (!body)
          return generation_error (*state.error, state.span);
        if (auto error = graph.add_equality (sig.result, *body, fn.span,
                                             "body of function '" + fn.name + "'"))
          return generation_error (*error, fn.span);
      }

    {
      const std::optional<structural::TermId> body =
          generate (graph, {}, signatures, program.body, state);
      if (!body)
        return generation_error (*state.error, state.span);
      program_result_ = *body;
    }

    structural::StructuralResult result = graph.solve (options);
    graph_ = std::move (graph);
    return result;
  }

  structural::TermId binding_term (const std::string &name) const
  {
    if (auto it = bindings_.find (name); it != bindings_.end ())
      return it->second;
    return no_term;
  }

  structural::TermId program_result () const { return program_result_; }
  const structural::TypeGraph &graph () const { return graph_; }
  std::optional<structural::StructuralType> type_of (structural::TermId term) const
  {
    return graph_.type_of (term);
  }

  static constexpr structural::TermId no_term = -1;

private:
  struct Signature
  {
    std::vector<structural::TermId> params;
    structural::TermId result = no_term;
  };

  struct Generation
  {
    std::optional<std::string> error;
    SourceSpan span{};
  };

  structural::StructuralResult generation_error (const std::string &message, SourceSpan span)
  {
    structural::StructuralResult result;
    push_diagnostic (result.base, "nerotypical.generation", Severity::Error, message, span);
    return result;
  }

  bool eq_checked (structural::TypeGraph &graph, Generation &state, structural::TermId left,
                   structural::TermId right, SourceSpan span, std::string note)
  {
    if (auto error = graph.add_equality (left, right, span, std::move (note)))
      {
        state.error = *error;
        state.span = span;
        return false;
      }
    return true;
  }

  std::optional<structural::TermId> generate (
      structural::TypeGraph &graph, const std::map<std::string, structural::TermId> &env,
      const std::map<std::string, Signature> &signatures, const ast::Expr &expr, Generation &state)
  {
    if (state.error)
      return std::nullopt;

    auto ground_of = [&](const char *name) -> structural::TermId {
      return graph.ground (*constructor_id (name));
    };

    switch (expr.kind)
      {
      case ast::Expr::Kind::IntLiteral:
        return ground_of ("int");
      case ast::Expr::Kind::FloatLiteral:
        return ground_of ("float");
      case ast::Expr::Kind::BoolLiteral:
        return ground_of ("bool");
      case ast::Expr::Kind::StringLiteral:
        return ground_of ("string");
      case ast::Expr::Kind::UnitLiteral:
        return ground_of ("unit");
      case ast::Expr::Kind::Variable:
        {
          auto it = env.find (expr.name);
          if (it == env.end ())
            {
              state.error = "unbound variable '" + expr.name + "'";
              state.span = expr.span;
              return std::nullopt;
            }
          return it->second;
        }
      case ast::Expr::Kind::Let:
        {
          const auto value = generate (graph, env, signatures, expr.children.at (0), state);
          if (!value)
            return std::nullopt;
          std::map<std::string, structural::TermId> inner = env;
          inner[expr.name] = *value;
          bindings_[expr.name] = *value;
          return generate (graph, inner, signatures, expr.children.at (1), state);
        }
      case ast::Expr::Kind::If:
        {
          const auto condition = generate (graph, env, signatures, expr.children.at (0), state);
          if (!condition)
            return std::nullopt;
          if (!eq_checked (graph, state, *condition, ground_of ("bool"),
                           expr.children.at (0).span, "if condition must be bool"))
            return std::nullopt;
          const auto then_branch = generate (graph, env, signatures, expr.children.at (1), state);
          if (!then_branch)
            return std::nullopt;
          const auto else_branch = generate (graph, env, signatures, expr.children.at (2), state);
          if (!else_branch)
            return std::nullopt;
          const structural::TermId result = graph.fresh_variable ("if result");
          if (!eq_checked (graph, state, result, *then_branch, expr.span, "then branch"))
            return std::nullopt;
          if (!eq_checked (graph, state, result, *else_branch, expr.span, "else branch"))
            return std::nullopt;
          return result;
        }
      case ast::Expr::Kind::Binary:
        {
          const ast::BinOp op = ast::binary_op (expr);
          const auto lhs = generate (graph, env, signatures, expr.children.at (0), state);
          if (!lhs)
            return std::nullopt;
          const auto rhs = generate (graph, env, signatures, expr.children.at (1), state);
          if (!rhs)
            return std::nullopt;
          const bool arithmetic = op == ast::BinOp::Add || op == ast::BinOp::Sub ||
                                  op == ast::BinOp::Mul || op == ast::BinOp::Div;
          const bool comparison = op == ast::BinOp::Lt || op == ast::BinOp::Le ||
                                  op == ast::BinOp::Gt || op == ast::BinOp::Ge;
          const bool logical = op == ast::BinOp::And || op == ast::BinOp::Or;
          if (arithmetic || comparison || logical)
            {
              const char *operand_type = arithmetic || comparison ? "int" : "bool";
              const char *note = arithmetic ? "arithmetic operand must be int"
                                            : comparison ? "comparison operand must be int"
                                                         : "logical operand must be bool";
              for (const structural::TermId operand : { *lhs, *rhs })
                if (!eq_checked (graph, state, operand, ground_of (operand_type), expr.span, note))
                  return std::nullopt;
            }
          const structural::TermId result = graph.fresh_variable ();
          const char *result_type = arithmetic ? "int" : "bool";
          if (!eq_checked (graph, state, result, ground_of (result_type), expr.span,
                           arithmetic ? "arithmetic result is int" : "operator result is bool"))
            return std::nullopt;
          return result;
        }
      case ast::Expr::Kind::Unary:
        {
          const auto operand = generate (graph, env, signatures, expr.children.at (0), state);
          if (!operand)
            return std::nullopt;
          const structural::TermId result = graph.fresh_variable ();
          if (ast::unary_op (expr) == ast::UnOp::Neg)
            {
              if (!eq_checked (graph, state, *operand, ground_of ("int"), expr.span,
                               "negation operand must be int"))
                return std::nullopt;
              if (!eq_checked (graph, state, result, ground_of ("int"), expr.span,
                               "negation result is int"))
                return std::nullopt;
              return result;
            }
          if (!eq_checked (graph, state, *operand, ground_of ("bool"), expr.span,
                           "not operand must be bool"))
            return std::nullopt;
          if (!eq_checked (graph, state, result, ground_of ("bool"), expr.span,
                           "not result is bool"))
            return std::nullopt;
          return result;
        }
      case ast::Expr::Kind::Call:
        {
          auto it = signatures.find (expr.name);
          if (it == signatures.end ())
            {
              state.error = "call to undeclared function '" + expr.name + "'";
              state.span = expr.span;
              return std::nullopt;
            }
          const Signature &sig = it->second;
          if (expr.children.size () != sig.params.size ())
            {
              state.error = "call to '" + expr.name + "' with " +
                            std::to_string (expr.children.size ()) + " arguments, expected " +
                            std::to_string (sig.params.size ());
              state.span = expr.span;
              return std::nullopt;
            }
          for (std::size_t i = 0; i < expr.children.size (); ++i)
            {
              const auto argument =
                  generate (graph, env, signatures, expr.children.at (i), state);
              if (!argument)
                return std::nullopt;
              if (!eq_checked (graph, state, *argument, sig.params[i],
                               expr.children.at (i).span,
                               "argument " + std::to_string (i + 1) + " of '" + expr.name + "'"))
                return std::nullopt;
            }
          return sig.result;
        }
      case ast::Expr::Kind::Lambda:
        {
          const structural::TermId param = graph.fresh_variable (expr.name);
          std::map<std::string, structural::TermId> inner = env;
          inner[expr.name] = param;
          bindings_[expr.name] = param;
          const auto body = generate (graph, inner, signatures, expr.children.at (0), state);
          if (!body)
            return std::nullopt;
          return graph.apply (*constructor_id ("fun"), { param, *body }, expr.span, "lambda");
        }
      case ast::Expr::Kind::Apply:
        {
          const auto fn = generate (graph, env, signatures, expr.children.at (0), state);
          if (!fn)
            return std::nullopt;
          const auto argument = generate (graph, env, signatures, expr.children.at (1), state);
          if (!argument)
            return std::nullopt;
          const structural::TermId domain = graph.fresh_variable ();
          const structural::TermId codomain = graph.fresh_variable ();
          const structural::TermId shape =
              graph.apply (*constructor_id ("fun"), { domain, codomain }, expr.span, "application");
          if (!eq_checked (graph, state, *fn, shape, expr.span, "applied value must be a function"))
            return std::nullopt;
          if (!eq_checked (graph, state, *argument, domain, expr.span, "applied argument"))
            return std::nullopt;
          return codomain;
        }
      }
    state.error = "unknown expression kind";
    state.span = expr.span;
    return std::nullopt;
  }

  std::vector<std::pair<std::string, int> > registry_{};
  std::map<std::string, structural::TermId> bindings_{};
  structural::TermId program_result_ = no_term;
  structural::TypeGraph graph_{};
};

} // namespace moonstone::nerotypical::language
