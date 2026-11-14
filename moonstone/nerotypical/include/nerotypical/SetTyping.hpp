#pragma once

// Nerotypical Mode A — set typing (soft typing) over a finite type universe.
//
// A "type" here is a set of atoms drawn from a finite, embedder-registered
// universe (int, float, bool, string, unit, ...).  Every type variable `x`
// is encoded by propositional variables b(x, i) meaning "atom i is in x",
// which makes the CNF encoding below *exact* (sound and complete) for the
// constraint vocabulary:
//
//   Equal(a, b)        a = b        (¬a ∨ b), (¬b ∨ a)           per atom
//   Subtype(a, b)      a ⊆ b        (¬a ∨ b)                    per atom
//   Union              r = a ∪ b    (¬a ∨ r), (¬b ∨ r), (¬r ∨ a ∨ b)
//   Intersect          r = a ∩ b    (¬a ∨ ¬b ∨ r), (¬r ∨ a), (¬r ∨ b)
//   Complement         r = U ∖ a    (¬r ∨ ¬a), (r ∨ a)
//   Disjoint(a, b)     a ∩ b = ∅    (¬a ∨ ¬b)
//   Inhabited(a)       a ≠ ∅        (a₁ ∨ … ∨ a_n)
//   Empty(a)           a = ∅        (¬a_i)                      per atom
//
// Constant terms inline their truth values into the clauses.  Solving is
// delegated to Satie (CDCL by default; DPLL and the exhaustive Native
// engine are selectable).  Error localization uses deletion-based core
// minimization: constraints are dropped one at a time and the remainder
// re-solved; a removal survives only if the system stays unsatisfiable,
// yielding an irreducible conflicting subset.  (The satie CDCL
// `solve_under` assumption interface was prototyped for conflict-directed
// cores but hangs on trivial selector-guarded formulas in the vendored
// revision — see `tests/set_typing.cpp` for the recorded repro — so
// Nerotypical deliberately does not call it.)  A satisfying model is
// reported as a *witness typing* (a consistent typing, not a principal
// one); the opt-in `precise_typing` pass computes exact per-(variable,
// atom) must/may sets with additional unit-clause solver queries.
// Budgets are enforced honestly: exhausted budgets set flags or return
// `Unknown`, never `WellTyped`.

#include "Analysis.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace moonstone::nerotypical::settype {

using AtomId = std::uint32_t;
using TypeVarId = std::uint32_t;

/// Sorted, duplicate-free atom identifiers.
using AtomSet = std::vector<AtomId>;

inline void normalize_atoms (AtomSet &atoms)
{
  std::sort (atoms.begin (), atoms.end ());
  atoms.erase (std::unique (atoms.begin (), atoms.end ()), atoms.end ());
}

/// Registry of flat type atoms.  Atom ids are stable for the lifetime of
/// the universe and assigned in interning order, so rendering (which walks
/// ids in order) is deterministic.
class TypeUniverse
{
public:
  AtomId intern (std::string name)
  {
    if (auto it = index_.find (name); it != index_.end ())
      return it->second;
    AtomId id = static_cast<AtomId> (names_.size ());
    names_.push_back (std::move (name));
    index_.emplace (names_.back (), id);
    return id;
  }

  std::optional<AtomId> find (std::string_view name) const
  {
    if (auto it = index_.find (std::string (name)); it != index_.end ())
      return it->second;
    return std::nullopt;
  }

  /// Throws std::out_of_range for unknown ids.
  const std::string &name (AtomId id) const { return names_.at (id); }
  const std::vector<std::string> &names () const { return names_; }
  std::size_t size () const { return names_.size (); }
  bool empty () const { return names_.empty (); }

private:
  std::vector<std::string> names_;
  std::unordered_map<std::string, AtomId> index_;
};

inline std::string render_atoms (const TypeUniverse &universe, const AtomSet &atoms)
{
  std::string out = "{";
  for (std::size_t i = 0; i < atoms.size (); ++i)
    {
      if (i != 0)
        out += ", ";
      out += universe.name (atoms[i]);
    }
  return out + "}";
}

/// A set-typing term: a type variable or a constant atom set.
struct TypeTerm
{
  enum class Kind : std::uint8_t
  {
    Variable,
    Constant
  };

  Kind kind = Kind::Constant;
  TypeVarId variable = 0;  // Kind::Variable
  AtomSet atoms;           // Kind::Constant (normalized)
};

inline TypeTerm variable_term (TypeVarId variable)
{
  TypeTerm t;
  t.kind = TypeTerm::Kind::Variable;
  t.variable = variable;
  return t;
}

inline TypeTerm constant_term (AtomSet atoms)
{
  TypeTerm t;
  t.kind = TypeTerm::Kind::Constant;
  normalize_atoms (atoms);
  t.atoms = std::move (atoms);
  return t;
}

/// One typing constraint.  Field usage by kind:
///   Equal      a = b            Subtype    a ⊆ b
///   Union      result = a ∪ b    Intersect  result = a ∩ b
///   Complement result = U ∖ a    Disjoint   a ∩ b = ∅
///   Inhabited  a ≠ ∅             Empty      a = ∅
/// `span` and `note` record provenance for diagnostics.
struct TypeConstraint
{
  enum class Kind : std::uint8_t
  {
    Equal,
    Subtype,
    Union,
    Intersect,
    Complement,
    Disjoint,
    Inhabited,
    Empty
  };

  Kind kind = Kind::Equal;
  TypeTerm a{}, b{}, result{};
  SourceSpan span{};
  std::string note;
};

/// Constraint set typing result.  `witness` is indexed by type variable id
/// and valid only when `base.status` is WellTyped; it is *a* consistent
/// typing, not a principal one.  `must_atoms`/`may_atoms` are the exact
/// "in every well-typing" / "in some well-typing" sets; they are filled
/// only when `AnalysisOptions::precise_typing` was requested, and may be
/// truncated when `base.stats.precision_incomplete` is set.
struct SetTypingResult
{
  BaseAnalysis base;
  std::vector<AtomSet> witness;
  std::vector<AtomSet> must_atoms;
  std::vector<AtomSet> may_atoms;
};

/// A finite set-typing problem: universe + type variables + constraints.
class ConstraintSystem
{
public:
  ConstraintSystem () = default;
  explicit ConstraintSystem (TypeUniverse universe) : universe_ (std::move (universe)) {}

  const TypeUniverse &universe () const { return universe_; }
  AtomId atom (std::string_view name) { return universe_.intern (std::string (name)); }
  std::optional<AtomId> atom_id (std::string_view name) const { return universe_.find (name); }

  TypeVarId fresh_variable (std::string display = {})
  {
    TypeVarId id = static_cast<TypeVarId> (variable_names_.size ());
    variable_names_.push_back (std::move (display));
    return id;
  }

  std::size_t variable_count () const { return variable_names_.size (); }
  /// Display name, or "t<id>" when the variable was created without one.
  /// Throws std::out_of_range for unknown ids.
  std::string variable_name (TypeVarId id) const
  {
    const std::string &name = variable_names_.at (id);
    return name.empty () ? "t" + std::to_string (id) : name;
  }

  /// Adds a constraint after validating it.  Returns nullopt on success;
  /// otherwise returns a human-readable reason and stores nothing.
  std::optional<std::string> add (TypeConstraint constraint)
  {
    if (auto error = validate (constraint))
      return error;
    constraints_.push_back (std::move (constraint));
    return std::nullopt;
  }

  std::size_t constraint_count () const { return constraints_.size (); }
  /// Throws std::out_of_range for unknown indices.
  const TypeConstraint &constraint (std::size_t index) const { return constraints_.at (index); }

  std::string describe (std::size_t index) const { return describe (constraints_.at (index)); }

  /// The exact CNF encoding of the system.  `active` optionally masks
  /// constraints (nullptr = all active); masked-off constraints contribute
  /// no clauses.
  struct Encoding
  {
    satie::CNF cnf;
    std::vector<std::vector<satie::Var>> bits;  // [variable][atom] -> propositional var
    satie::Var next_variable = 0;              // 1-based; 0 when the system is empty
  };

  Encoding encode (const std::vector<unsigned char> *active = nullptr) const
  {
    Encoding e;
    const std::size_t n = universe_.size ();
    const std::size_t v = variable_names_.size ();
    e.bits.assign (v, std::vector<satie::Var> (n));
    for (std::size_t vi = 0; vi < v; ++vi)
      for (std::size_t ai = 0; ai < n; ++ai)
        e.bits[vi][ai] = static_cast<satie::Var> (1 + vi * n + ai);
    satie::Var next = static_cast<satie::Var> (1 + v * n);

    struct AtomBit
    {
      bool constant = true;
      bool value = false;
      satie::Var var = 0;
    };
    auto bit_for = [&e] (const TypeTerm &t, AtomId i) -> AtomBit {
      if (t.kind == TypeTerm::Kind::Variable)
        return { false, false, e.bits.at (t.variable).at (i) };
      const AtomSet &atoms = t.atoms;
      return { true, std::binary_search (atoms.begin (), atoms.end (), i), 0 };
    };
    auto emit = [&e] (std::vector<std::pair<AtomBit, bool> > lits) {
      satie::Clause clause;
      for (auto &entry : lits)
        {
          if (entry.first.constant)
            {
              if (entry.second == entry.first.value)
                return;  // constant satisfies the clause
              continue;  // constant falsifies its literal
            }
          clause.push_back (entry.second ? entry.first.var : -entry.first.var);
        }
      e.cnf.add_clause (std::move (clause));
    };

    for (std::size_t k = 0; k < constraints_.size (); ++k)
      {
        if (active && !active->at (k))
          continue;
        const TypeConstraint &c = constraints_[k];
        const TypeTerm &a = c.a;
        const TypeTerm &b = c.b;
        switch (c.kind)
          {
          case TypeConstraint::Kind::Equal:
            for (AtomId i = 0; i < n; ++i)
              {
                emit ({ { bit_for (a, i), false }, { bit_for (b, i), true } });
                emit ({ { bit_for (b, i), false }, { bit_for (a, i), true } });
              }
            break;
          case TypeConstraint::Kind::Subtype:
            for (AtomId i = 0; i < n; ++i)
              emit ({ { bit_for (a, i), false }, { bit_for (b, i), true } });
            break;
          case TypeConstraint::Kind::Union:
            for (AtomId i = 0; i < n; ++i)
              {
                emit ({ { bit_for (a, i), false }, { bit_for (c.result, i), true } });
                emit ({ { bit_for (b, i), false }, { bit_for (c.result, i), true } });
                emit ({ { bit_for (c.result, i), false }, { bit_for (a, i), true },
                                   { bit_for (b, i), true } });
              }
            break;
          case TypeConstraint::Kind::Intersect:
            for (AtomId i = 0; i < n; ++i)
              {
                emit ({ { bit_for (a, i), false }, { bit_for (b, i), false },
                                   { bit_for (c.result, i), true } });
                emit ({ { bit_for (c.result, i), false }, { bit_for (a, i), true } });
                emit ({ { bit_for (c.result, i), false }, { bit_for (b, i), true } });
              }
            break;
          case TypeConstraint::Kind::Complement:
            for (AtomId i = 0; i < n; ++i)
              {
                emit ({ { bit_for (c.result, i), false }, { bit_for (a, i), false } });
                emit ({ { bit_for (c.result, i), true }, { bit_for (a, i), true } });
              }
            break;
          case TypeConstraint::Kind::Disjoint:
            for (AtomId i = 0; i < n; ++i)
              emit ({ { bit_for (a, i), false }, { bit_for (b, i), false } });
            break;
          case TypeConstraint::Kind::Inhabited:
            if (a.kind == TypeTerm::Kind::Constant)
              {
                if (a.atoms.empty ())
                  emit ({});  // empty constant type: contradiction
              }
            else
              {
                std::vector<std::pair<AtomBit, bool> > lits;
                lits.reserve (n);
                for (AtomId i = 0; i < n; ++i)
                  lits.push_back ({ bit_for (a, i), true });
                emit (std::move (lits));
              }
            break;
          case TypeConstraint::Kind::Empty:
            if (a.kind == TypeTerm::Kind::Constant)
              {
                if (!a.atoms.empty ())
                  emit ({});  // nonempty constant cannot be empty
              }
            else
              {
                for (AtomId i = 0; i < n; ++i)
                  emit ({ { bit_for (a, i), false } });
              }
            break;
          }
      }

    e.next_variable = next;
    if (next > 1)
      e.cnf.set_declared_variable_count (static_cast<std::size_t> (next - 1));
    return e;
  }

  /// Solves the system with Satie.  See the header comment for the exact
  /// semantics of each status.
  SetTypingResult solve (const AnalysisOptions &options = {}) const
  {
    SetTypingResult result;
    BaseAnalysis &base = result.base;
    base.stats.engine = std::string (satie::engine_name (options.engine));

    const std::uint64_t n = universe_.size ();
    const std::uint64_t v = variable_names_.size ();
    const std::uint64_t count = constraints_.size ();

    auto invalid = [&] (const std::string &message) {
      base.stats.solver_calls = 0;
      push_diagnostic (base, "nerotypical.invalid_input", Severity::Error, message, SourceSpan{});
      return result;
    };
    if (universe_.empty ())
      return invalid ("type universe is empty; register atoms before solving");
    if (n > options.max_atoms)
      return invalid ("type universe has " + std::to_string (n) +
                      " atoms, exceeding max_atoms=" + std::to_string (options.max_atoms));
    if (1 + v * n + count + 2 >
        static_cast<std::uint64_t> (std::numeric_limits<satie::Var>::max ()))
      return invalid ("encoding size exceeds the satie variable range");
    if (options.max_solver_calls == 0)
      {
        base.status = AnalysisStatus::Unknown;
        base.stats.solver_calls = 0;
        push_diagnostic (base, "nerotypical.unknown", Severity::Warning,
                         "solver call budget is zero; no well-typedness claim is made",
                         SourceSpan{});
        return result;
      }

    const Encoding e = encode ();
    base.stats.cnf_variables = e.cnf.variable_count ();
    base.stats.cnf_clauses = e.cnf.clause_count ();

    std::size_t calls = 0;
    auto budget_left = [&] { return calls < options.max_solver_calls; };
    auto run = [&](const satie::CNF &cnf) -> satie::SolveResult {
      ++calls;
      switch (options.engine)
        {
        case satie::Engine::CDCL: return satie::solve_cdcl (cnf);
        case satie::Engine::DPLL: return satie::solve_dpll (cnf);
        case satie::Engine::Native: break;
        }
      return satie::solve_naive (cnf);
    };

    // One additional solver query with a forced bit value: the literal is
    // asserted as a unit clause on a copy of the encoding.  Returns
    // nullopt when the call budget is exhausted before the query could
    // run.  (The satie CDCL assumption interface is not used; see the
    // header comment.)
    auto query = [&](satie::Lit lit) -> std::optional<satie::SolveStatus> {
      if (!budget_left ())
        return std::nullopt;
      satie::CNF restricted = e.cnf;
      restricted.add_clause (satie::Clause{ lit });
      return run (restricted).status;
    };

    const satie::SolveResult solution = run (e.cnf);
    if (solution.status == satie::SolveStatus::UNSAT)
      {
        // Deletion-based core minimization: try removing each constraint;
        // keep the removal only when the remainder stays unsatisfiable.
        // One solver call per trial, budget-guarded.
        std::vector<unsigned char> active (constraints_.size (), 1);
        bool incomplete = false;
        for (std::size_t k = 0; k < constraints_.size (); ++k)
          {
            if (!budget_left ())
              {
                incomplete = true;
                break;
              }
            active[k] = 0;
            const Encoding sub = encode (&active);
            const satie::SolveStatus trial = run (sub.cnf).status;
            if (trial != satie::SolveStatus::UNSAT)
              active[k] = 1;
          }
        std::vector<std::size_t> core;
        for (std::size_t k = 0; k < constraints_.size (); ++k)
          if (active[k])
            core.push_back (k);
        report_type_error (base, core, incomplete);
        base.stats.solver_calls = calls;
        return result;
      }

    base.stats.solver_calls = calls;
    if (solution.status != satie::SolveStatus::SAT)
      {
        base.status = AnalysisStatus::Unknown;
        push_diagnostic (base, "nerotypical.unknown", Severity::Warning,
                         "the solver returned UNKNOWN; no well-typedness claim is made",
                         SourceSpan{});
        return result;
      }

    // SAT: extract a witness typing.
    base.status = AnalysisStatus::WellTyped;
    result.witness.assign (variable_names_.size (), AtomSet{});
    for (std::size_t vi = 0; vi < variable_names_.size (); ++vi)
      for (std::size_t ai = 0; ai < n; ++ai)
        if (solution.assignment.get_var (e.bits[vi][ai]) == satie::Value::TRUE)
          result.witness[vi].push_back (static_cast<AtomId> (ai));

    if (options.precise_typing)
      {
        result.must_atoms.assign (variable_names_.size (), AtomSet{});
        result.may_atoms.assign (variable_names_.size (), AtomSet{});
        bool truncated = false;
        for (std::size_t vi = 0; vi < variable_names_.size () && !truncated; ++vi)
          for (std::size_t ai = 0; ai < n; ++ai)
            {
              const satie::Lit bit = e.bits[vi][ai];
              auto must_query = query (-bit);
              if (!must_query || *must_query == satie::SolveStatus::UNKNOWN)
                {
                  truncated = true;
                  break;
                }
              bool has_must = false;
              if (*must_query == satie::SolveStatus::UNSAT)
                {
                  result.must_atoms[vi].push_back (static_cast<AtomId> (ai));
                  has_must = true;
                }
              const bool witness_bit =
                  std::binary_search (result.witness[vi].begin (), result.witness[vi].end (),
                                      static_cast<AtomId> (ai));
              if (has_must || witness_bit)
                result.may_atoms[vi].push_back (static_cast<AtomId> (ai));
              else
                {
                  auto may_query = query (bit);
                  if (!may_query || *may_query == satie::SolveStatus::UNKNOWN)
                    {
                      truncated = true;
                      break;
                    }
                  if (*may_query == satie::SolveStatus::SAT)
                    result.may_atoms[vi].push_back (static_cast<AtomId> (ai));
                }
            }
        base.stats.precision_incomplete = truncated;
        if (truncated)
          push_diagnostic (base, "nerotypical.precision.budget", Severity::Note,
                           "precision pass truncated by the solver call budget; must/may sets are partial",
                           SourceSpan{});
      }

    base.stats.solver_calls = calls;
    return result;
  }

  /// Renders a term: variable display name or a constant atom set.
  std::string describe_term (const TypeTerm &term) const
  {
    if (term.kind == TypeTerm::Kind::Variable)
      return variable_name (term.variable);
    return render_atoms (universe_, term.atoms);
  }

  std::string describe (const TypeConstraint &c) const
  {
    switch (c.kind)
      {
      case TypeConstraint::Kind::Equal:
        return describe_term (c.a) + " = " + describe_term (c.b);
      case TypeConstraint::Kind::Subtype:
        return describe_term (c.a) + " <= " + describe_term (c.b);
      case TypeConstraint::Kind::Union:
        return describe_term (c.result) + " = " + describe_term (c.a) + " | " +
               describe_term (c.b);
      case TypeConstraint::Kind::Intersect:
        return describe_term (c.result) + " = " + describe_term (c.a) + " & " +
               describe_term (c.b);
      case TypeConstraint::Kind::Complement:
        return describe_term (c.result) + " = ~" + describe_term (c.a);
      case TypeConstraint::Kind::Disjoint:
        return describe_term (c.a) + " & " + describe_term (c.b) + " is empty";
      case TypeConstraint::Kind::Inhabited:
        return describe_term (c.a) + " is inhabited";
      case TypeConstraint::Kind::Empty:
        return describe_term (c.a) + " is empty";
      }
    return describe_term (c.a);
  }

private:
  std::optional<std::string> validate (const TypeConstraint &c) const
  {
    const std::size_t atoms = universe_.size ();
    const std::size_t variables = variable_names_.size ();
    const auto check_term = [&](const TypeTerm &t) -> std::optional<std::string> {
      if (t.kind == TypeTerm::Kind::Variable)
        {
          if (t.variable >= variables)
            return "constraint references type variable " + std::to_string (t.variable) +
                   " but only " + std::to_string (variables) + " exist";
          return std::nullopt;
        }
      for (AtomId id : t.atoms)
        if (id >= atoms)
          return "constraint references atom id " + std::to_string (id) +
                 " but the universe has " + std::to_string (atoms) + " atoms";
      return std::nullopt;
    };
    if (auto error = check_term (c.a))
      return error;
    if (auto error = check_term (c.b))
      return error;
    if (auto error = check_term (c.result))
      return error;
    return std::nullopt;
  }

  void report_type_error (BaseAnalysis &base, const std::vector<std::size_t> &core,
                          bool incomplete) const
  {
    base.status = AnalysisStatus::TypeError;
    base.stats.localization_incomplete = incomplete;
    const SourceSpan primary = core.empty () ? SourceSpan{} : constraints_.at (core.front ()).span;
    Diagnostic &d = push_diagnostic (base, "nerotypical.type.conflict", Severity::Error,
                                     "no consistent set typing exists for the given constraints",
                                     primary);
    for (std::size_t k : core)
      {
        const TypeConstraint &c = constraints_.at (k);
        d.related.push_back (c.span);
        d.notes.push_back (describe (k) + (c.note.empty () ? "" : " — " + c.note));
        ConflictReport report;
        report.kind = "constraint";
        report.description = describe (k);
        report.span = c.span;
        report.note = c.note;
        base.conflict.push_back (std::move (report));
      }
  }

  TypeUniverse universe_{};
  std::vector<std::string> variable_names_{};
  std::vector<TypeConstraint> constraints_{};
};

} // namespace moonstone::nerotypical::settype
