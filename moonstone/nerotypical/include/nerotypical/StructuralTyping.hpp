#pragma once

// Nerotypical Mode B — structural typing (unification) solved by Satie EUF.
//
// Types are terms over registered constructors: ground constructors such
// as int/bool/string/unit, structural constructors such as fun/list/pair,
// and type variables.  Constraints are equalities (plus optional explicit
// disequalities) between type terms.
//
// The solver is `satie::euf::EUFSolver`, i.e. congruence closure.  Plain
// congruence closure alone is *not* unification: equating two applications
// of the same constructor merges the applications but leaves their
// arguments unconstrained, which would silently accept programs a
// unifier rejects (unsound for a type checker).  Nerotypical therefore
// runs a decomposition fixpoint driver on top of the closure:
//
//   1. run closure (`euf.check()`),
//   2. within each equivalence class, find two applications of the same
//      constructor whose argument classes differ and assert the argument
//      equalities,
//   3. repeat until no new equalities are produced.
//
// Every round adds an equality between terms that are in *different*
// classes, so the number of classes strictly decreases and the loop
// terminates (bounded by `max_unification_rounds`, reported as `Unknown`
// when exceeded — never as well-typed).
//
// After the fixpoint the classes are validated: a class containing
// applications of two *distinct* constructors is a type clash; a class
// whose application graph reaches itself is a recursive type, rejected
// unless `allow_recursive_types` is set (then readback marks the cycle).
// Well-typed systems get an inferred type per variable, read back from
// the classes.

#include "Analysis.hpp"

#include <SatieEUF.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace moonstone::nerotypical::structural {

using TermId = int;
using ConstructorId = int;

/// A type read back from the solved equivalence classes.  `Rec` marks a
/// recursive back-edge; it only appears when recursive types were allowed.
struct StructuralType
{
  enum class Kind : std::uint8_t
  {
    Variable,
    Application,
    Rec
  };

  Kind kind = Kind::Variable;
  std::string name;
  std::vector<StructuralType> args;

  std::string render () const
  {
    switch (kind)
      {
      case Kind::Variable: return "'" + name;
      case Kind::Rec: return "mu " + name;
      case Kind::Application:
        if (args.empty ())
          return name;
        std::string out = name + " (";
        for (std::size_t i = 0; i < args.size (); ++i)
          {
            if (i != 0)
              out += ", ";
            out += args[i].render ();
          }
        return out + ")";
      }
    return "'" + name;
  }
};

struct StructuralResult
{
  BaseAnalysis base;
  /// Inferred type of every type variable, in creation order.
  std::vector<std::pair<TermId, StructuralType> > inferred;
};

/// A structural typing problem backed by satie's EUF solver.
class TypeGraph
{
public:
  TypeGraph () = default;

  /// Registers a type constructor.  Returns an error message on an empty
  /// name, a duplicate name or a negative arity; otherwise nullopt.
  std::optional<std::string> add_constructor (std::string name, int arity)
  {
    if (name.empty ())
      return "constructor name must not be empty";
    if (arity < 0)
      return "constructor " + name + " has a negative arity";
    if (constructor_index_.count (name) != 0)
      return "constructor " + name + " is already registered";
    const ConstructorId id = euf_.add_func (name, arity);
    constructors_.push_back (ConstructorInfo{ std::move (name), arity });
    constructor_index_.emplace (constructors_.back ().name, id);
    return std::nullopt;
  }

  std::size_t constructor_count () const { return constructors_.size (); }
  /// Throws std::out_of_range for unknown ids.
  const std::string &constructor_name (ConstructorId id) const
  {
    return constructors_.at (static_cast<std::size_t> (id)).name;
  }
  std::optional<ConstructorId> find_constructor (const std::string &name) const
  {
    if (auto it = constructor_index_.find (name); it != constructor_index_.end ())
      return it->second;
    return std::nullopt;
  }

  /// A fresh type variable.  `display` is used in diagnostics.
  TermId fresh_variable (std::string display = {})
  {
    const TermId id = euf_.add_var ("tv" + std::to_string (terms_.size ()));
    TermRecord record;
    record.kind = TermKind::Variable;
    record.display = std::move (display);
    terms_.push_back (std::move (record));
    return id;
  }

  /// The ground type of a zero-arity constructor (memoized per class of
  /// constructors).  Throws std::out_of_range for an unknown constructor or
  /// std::invalid_argument when the arity is not zero.
  TermId ground (ConstructorId id)
  {
    const ConstructorInfo &info = constructors_.at (static_cast<std::size_t> (id));
    if (info.arity != 0)
      throw std::invalid_argument ("constructor " + info.name + " is not a ground type");
    if (auto it = ground_terms_.find (id); it != ground_terms_.end ())
      return it->second;
    const TermId term = euf_.mk_app (id, {});
    TermRecord record;
    record.kind = TermKind::Application;
    record.ctor = id;
    record.span = SourceSpan{};
    terms_.push_back (std::move (record));
    ground_terms_.emplace (id, term);
    return term;
  }

  /// A constructor application.  `span` records the program provenance of
  /// the term for diagnostics.  Throws std::out_of_range/std::invalid_argument
  /// on unknown ids or arity mismatch (the underlying satie checks).
  TermId apply (ConstructorId id, std::vector<TermId> args, SourceSpan span = {},
                std::string note = {})
  {
    const TermId term = euf_.mk_app (id, args);
    TermRecord record;
    record.kind = TermKind::Application;
    record.ctor = id;
    record.args = std::move (args);
    record.span = span;
    record.note = std::move (note);
    terms_.push_back (std::move (record));
    return term;
  }

  std::size_t term_count () const { return terms_.size (); }

  /// Asserts t1 = t2.  Returns an error message for unknown terms.
  std::optional<std::string> add_equality (TermId t1, TermId t2, SourceSpan span = {},
                                           std::string note = {})
  {
    if (auto error = check_term (t1, "equality"))
      return error;
    if (auto error = check_term (t2, "equality"))
      return error;
    euf_.add_eq (t1, t2);
    equalities_.push_back (EqualityRecord{ t1, t2, span, std::move (note), true });
    return std::nullopt;
  }

  /// Asserts t1 != t2.  Violations are reported by `solve`.
  std::optional<std::string> add_disequality (TermId t1, TermId t2, SourceSpan span = {},
                                              std::string note = {})
  {
    if (auto error = check_term (t1, "disequality"))
      return error;
    if (auto error = check_term (t2, "disequality"))
      return error;
    euf_.add_diseq (t1, t2);
    disequalities_.push_back (EqualityRecord{ t1, t2, span, std::move (note), true });
    return std::nullopt;
  }

  /// Renders a term as written (pre-unification shape).
  std::string describe_term (TermId id) const
  {
    const TermRecord &record = terms_.at (static_cast<std::size_t> (id));
    if (record.kind == TermKind::Variable)
      return record.display.empty () ? "'a" + std::to_string (id) : "'" + record.display;
    std::string out = constructor_name (record.ctor);
    if (record.args.empty ())
      return out;
    out += " (";
    for (std::size_t i = 0; i < record.args.size (); ++i)
      {
        if (i != 0)
          out += ", ";
        out += describe_term (record.args[i]);
      }
    return out + ")";
  }

  bool solved () const { return solved_; }

  /// The unified type of a term; valid after a successful `solve`.
  std::optional<StructuralType> type_of (TermId id) const
  {
    if (!solved_ || id < 0 || static_cast<std::size_t> (id) >= terms_.size ())
      return std::nullopt;
    std::set<int> path;
    std::map<int, StructuralType> memo;
    return read_class (euf_.representative (id), path, memo);
  }

  /// Runs the unification fixpoint and validates the result.  See the
  /// header comment for the semantics of each status.
  StructuralResult solve (const AnalysisOptions &options = {})
  {
    StructuralResult result;
    BaseAnalysis &base = result.base;
    base.stats.engine = "euf";
    base.stats.type_terms = terms_.size ();
    solved_ = false;

    if (terms_.size () > options.max_type_terms)
      {
        push_diagnostic (base, "nerotypical.invalid_input", Severity::Error,
                         "type graph has " + std::to_string (terms_.size ()) +
                             " terms, exceeding max_type_terms=" +
                             std::to_string (options.max_type_terms),
                         SourceSpan{});
        return result;
      }

    // --- decomposition fixpoint over congruence closure ------------------
    std::size_t rounds = 0;
    bool changed = true;
    while (changed)
      {
        if (++rounds > options.max_unification_rounds)
          {
            base.stats.unification_rounds = rounds;
            base.status = AnalysisStatus::Unknown;
            push_diagnostic (base, "nerotypical.unknown", Severity::Warning,
                             "unification round budget exhausted; no well-typedness claim is made",
                             SourceSpan{});
            return result;
          }
        const satie::SolveResult closure = euf_.check ();
        if (closure.unsatisfiable ())
          {
            base.stats.unification_rounds = rounds;
            report_disequality_violation (base);
            return result;
          }
        changed = decompose_once ();
      }
    base.stats.unification_rounds = rounds;

    // --- post-fixpoint validation ---------------------------------------
    if (!validate_disequalities (base))
      return result;
    if (!validate_clashes (base))
      return result;
    if (!options.allow_recursive_types && !validate_occurs (base))
      return result;

    solved_ = true;
    base.status = AnalysisStatus::WellTyped;
    for (std::size_t i = 0; i < terms_.size (); ++i)
      if (terms_[i].kind == TermKind::Variable)
        {
          const TermId id = static_cast<TermId> (i);
          std::set<int> path;
          std::map<int, StructuralType> memo;
          result.inferred.emplace_back (id, read_class (euf_.representative (id), path, memo));
        }
    // Allowed recursive types are surfaced as "mu recursive" markers in the
    // readback instead of diagnostics.
    return result;
  }

private:
  enum class TermKind : std::uint8_t
  {
    Variable,
    Application
  };

  struct TermRecord
  {
    TermKind kind = TermKind::Variable;
    ConstructorId ctor = -1;
    std::vector<TermId> args;
    std::string display;
    SourceSpan span;
    std::string note;
  };

  struct ConstructorInfo
  {
    std::string name;
    int arity = 0;
  };

  struct EqualityRecord
  {
    TermId left = -1;
    TermId right = -1;
    SourceSpan span;
    std::string note;
    bool user = true;
  };

  std::optional<std::string> check_term (TermId id, const char *what) const
  {
    if (id < 0 || static_cast<std::size_t> (id) >= terms_.size ())
      return std::string (what) + " references term " + std::to_string (id) + " but only " +
             std::to_string (terms_.size ()) + " exist";
    return std::nullopt;
  }

  /// One decomposition round: for every class, unify the arguments of two
  /// applications of the same constructor.  Returns true when new
  /// equalities were asserted.
  bool decompose_once ()
  {
    std::map<int, std::map<ConstructorId, std::vector<TermId> > > classes;
    for (std::size_t i = 0; i < terms_.size (); ++i)
      if (terms_[i].kind == TermKind::Application)
        {
          const TermId id = static_cast<TermId> (i);
          classes[euf_.representative (id)][terms_[i].ctor].push_back (id);
        }
    bool changed = false;
    for (const auto &[root, by_ctor] : classes)
      {
        (void)root;
        for (const auto &[ctor, apps] : by_ctor)
          {
            if (apps.size () < 2)
              continue;
            const TermId first = apps.front ();
            const TermRecord &first_record = terms_[static_cast<std::size_t> (first)];
            for (std::size_t i = 1; i < apps.size (); ++i)
              {
                const TermRecord &other = terms_[static_cast<std::size_t> (apps[i])];
                for (std::size_t j = 0; j < first_record.args.size (); ++j)
                  if (euf_.representative (first_record.args[j]) !=
                      euf_.representative (other.args[j]))
                    {
                      euf_.add_eq (first_record.args[j], other.args[j]);
                      equalities_.push_back (
                          EqualityRecord{ first_record.args[j], other.args[j],
                                          first_record.span,
                                          "decomposed from " + describe_term (first) + " = " +
                                              describe_term (apps[i]),
                                          false });
                      changed = true;
                    }
              }
          }
      }
    return changed;
  }

  /// All application terms whose class root is `root`, lowest id first.
  std::vector<TermId> class_applications (int root) const
  {
    std::vector<TermId> apps;
    for (std::size_t i = 0; i < terms_.size (); ++i)
      if (terms_[i].kind == TermKind::Application && euf_.representative (static_cast<TermId> (i)) == root)
        apps.push_back (static_cast<TermId> (i));
    return apps;
  }

  /// The lowest-id variable term in the class of `root`, if any.
  std::optional<TermId> class_variable (int root) const
  {
    for (std::size_t i = 0; i < terms_.size (); ++i)
      if (terms_[i].kind == TermKind::Variable &&
          euf_.representative (static_cast<TermId> (i)) == root)
        return static_cast<TermId> (i);
    return std::nullopt;
  }

  /// User and decomposed equalities whose endpoints live in this class.
  std::vector<const EqualityRecord *> class_equalities (int root) const
  {
    std::vector<const EqualityRecord *> out;
    for (const EqualityRecord &eq : equalities_)
      if (euf_.representative (eq.left) == root)
        out.push_back (&eq);
    return out;
  }

  bool validate_disequalities (BaseAnalysis &base)
  {
    bool violated = false;
    for (const EqualityRecord &eq : disequalities_)
      if (euf_.equal_terms (eq.left, eq.right))
        {
          violated = true;
          ConflictReport report;
          report.kind = "disequality";
          report.description = "cannot require " + describe_term (eq.left) + " != " +
                               describe_term (eq.right) + ": they unify";
          report.span = eq.span;
          report.note = eq.note;
          base.conflict.push_back (std::move (report));
          Diagnostic &d = push_diagnostic (base, "nerotypical.type.disequality", Severity::Error,
                                           "disequality between unified types is impossible",
                                           eq.span);
          d.notes.push_back (report.description);
        }
    if (violated)
      base.status = AnalysisStatus::TypeError;
    return !violated;
  }

  bool validate_clashes (BaseAnalysis &base)
  {
    std::set<int> roots;
    for (std::size_t i = 0; i < terms_.size (); ++i)
      if (terms_[i].kind == TermKind::Application)
        roots.insert (euf_.representative (static_cast<TermId> (i)));
    for (int root : roots)
      {
        const std::vector<TermId> apps = class_applications (root);
        for (std::size_t i = 0; i < apps.size (); ++i)
          for (std::size_t j = i + 1; j < apps.size (); ++j)
            if (terms_[static_cast<std::size_t> (apps[i])].ctor !=
                terms_[static_cast<std::size_t> (apps[j])].ctor)
              {
                const TermId left = apps[i];
                const TermId right = apps[j];
                const std::string description =
                    "cannot unify " + describe_term (left) + " with " + describe_term (right);
                ConflictReport report;
                report.kind = "type_clash";
                report.description = description;
                report.span = terms_[static_cast<std::size_t> (left)].span;
                report.note = terms_[static_cast<std::size_t> (right)].note;
                base.conflict.push_back (std::move (report));
                // Ground terms carry no provenance span (they are memoized
                // and shared), so prefer the span of the first user
                // equality that forced the clash; it identifies the
                // expression that brought the types together.
                SourceSpan primary = terms_[static_cast<std::size_t> (left)].span;
                for (const EqualityRecord *eq : class_equalities (root))
                  if (eq->user && (!eq->span.file.empty () || eq->span.line != 1))
                    {
                      primary = eq->span;
                      break;
                    }
                Diagnostic &d = push_diagnostic (base, "nerotypical.type.clash", Severity::Error,
                                                 description, primary);
                d.related.push_back (terms_[static_cast<std::size_t> (right)].span);
                for (const EqualityRecord *eq : class_equalities (root))
                  d.notes.push_back (describe_term (eq->left) + " = " + describe_term (eq->right) +
                                     (eq->note.empty () ? "" : " — " + eq->note));
                base.status = AnalysisStatus::TypeError;
                return false;
              }
      }
    return true;
  }

  /// Detects cycles in the class graph (class -> argument classes).  With
  /// `report_only` the conflicts are recorded but do not change a status.
  bool validate_occurs_impl (BaseAnalysis &base)
  {
    std::set<int> roots;
    for (std::size_t i = 0; i < terms_.size (); ++i)
      if (terms_[i].kind == TermKind::Application)
        roots.insert (euf_.representative (static_cast<TermId> (i)));

    std::set<int> done;
    std::set<int> on_path;
    std::function<void (int)> visit = [&] (int root) {
      if (done.count (root) != 0)
        return;
      if (on_path.count (root) != 0)
        {
          // cycle: the class contains itself through its arguments
          const std::vector<TermId> apps = class_applications (root);
          if (apps.empty ())
            return;
          const TermId app = apps.front ();
          const std::string description =
              "recursive type: " + describe_term (app) + " reaches itself";
          ConflictReport report;
          report.kind = "occurs_check";
          report.description = description;
          report.span = terms_[static_cast<std::size_t> (app)].span;
          base.conflict.push_back (std::move (report));
          Diagnostic &d = push_diagnostic (base, "nerotypical.type.occurs", Severity::Error,
                                           description,
                                           terms_[static_cast<std::size_t> (app)].span);
          for (const EqualityRecord *eq : class_equalities (root))
            d.notes.push_back (describe_term (eq->left) + " = " + describe_term (eq->right));
          return;
        }
      on_path.insert (root);
      for (const TermId app : class_applications (root))
        for (const TermId arg : terms_[static_cast<std::size_t> (app)].args)
          visit (euf_.representative (arg));
      on_path.erase (root);
      done.insert (root);
    };
    bool has_conflict = false;
    for (int root : roots)
      {
        const std::size_t before = base.conflict.size ();
        visit (root);
        if (base.conflict.size () > before)
          has_conflict = true;
      }
    return has_conflict;
  }

  bool validate_occurs (BaseAnalysis &base)
  {
    const bool has_conflict = validate_occurs_impl (base);
    if (has_conflict)
      base.status = AnalysisStatus::TypeError;
    return !has_conflict;
  }

  void report_disequality_violation (BaseAnalysis &base)
  {
    // The closure reported UNSAT; attribute it to the violated user
    // disequalities we can identify.
    bool attributed = false;
    for (const EqualityRecord &eq : disequalities_)
      if (euf_.equal_terms (eq.left, eq.right))
        {
          attributed = true;
          ConflictReport report;
          report.kind = "disequality";
          report.description = "cannot require " + describe_term (eq.left) + " != " +
                               describe_term (eq.right) + ": they unify";
          report.span = eq.span;
          report.note = eq.note;
          base.conflict.push_back (std::move (report));
          push_diagnostic (base, "nerotypical.type.disequality", Severity::Error,
                           report.description, eq.span);
        }
    if (!attributed)
      {
        base.status = AnalysisStatus::Unknown;
        push_diagnostic (base, "nerotypical.unknown", Severity::Warning,
                         "the EUF closure reported unsatisfiable without an attributable disequality",
                         SourceSpan{});
        return;
      }
    base.status = AnalysisStatus::TypeError;
  }

  /// Reads the unified type of a class as a term tree.
  StructuralType read_class (int root, std::set<int> &path,
                             std::map<int, StructuralType> &memo) const
  {
    if (auto it = memo.find (root); it != memo.end ())
      return it->second;
    if (path.count (root) != 0)
      {
        // Recursive back-edge; only reachable when recursive types are
        // allowed (or before the occurs check ran).
        StructuralType marker;
        marker.kind = StructuralType::Kind::Rec;
        marker.name = "recursive";
        return marker;
      }
    const std::vector<TermId> apps = class_applications (root);
    path.insert (root);
    StructuralType out;
    if (apps.empty ())
      {
        out.kind = StructuralType::Kind::Variable;
        if (const auto variable = class_variable (root))
          {
            const TermRecord &record = terms_[static_cast<std::size_t> (*variable)];
            out.name = record.display.empty () ? "a" + std::to_string (*variable)
                                               : record.display;
          }
        else
          out.name = "unsolved";
      }
    else
      {
        const TermId app = apps.front ();
        const TermRecord &record = terms_[static_cast<std::size_t> (app)];
        out.kind = StructuralType::Kind::Application;
        out.name = constructor_name (record.ctor);
        out.args.reserve (record.args.size ());
        for (const TermId arg : record.args)
          out.args.push_back (read_class (euf_.representative (arg), path, memo));
      }
    path.erase (root);
    memo.emplace (root, out);
    return out;
  }

  std::vector<ConstructorInfo> constructors_{};
  std::map<std::string, ConstructorId> constructor_index_{};
  std::unordered_map<ConstructorId, TermId> ground_terms_{};
  std::vector<TermRecord> terms_{};
  std::vector<EqualityRecord> equalities_{};
  std::vector<EqualityRecord> disequalities_{};
  satie::euf::EUFSolver euf_{};
  bool solved_ = false;
};

} // namespace moonstone::nerotypical::structural
