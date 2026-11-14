#pragma once

// Nerotypical — shared analysis vocabulary.
//
// Nerotypical provides type analysis for compilers and interpreters by
// generating typing constraints and solving them with the vendored Satie
// solver (third_party/satie).  Two solving modes share the result vocabulary
// declared here:
//
//   * settype     (SetTyping.hpp)      — union/set typing over a finite type
//                                        universe, encoded exactly into CNF
//                                        and decided by Satie CDCL/DPLL.
//   * structural  (StructuralTyping.hpp) — classic unification over type
//                                        terms, decided by Satie's EUF
//                                        congruence closure plus a
//                                        decomposition driver.
//
// Statuses are never silently upgraded: an engine answer of UNKNOWN, an
// exhausted budget or malformed input is reported as such and never
// interpreted as "well typed" (Moonstone invariant: budgets and unknowns do
// not authorize).

#if __has_include("../../../core/Core.hpp")
#include "../../../core/Core.hpp"
#else
#include <moonstone/core/Core.hpp>
#endif

#include <Satie.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace moonstone::nerotypical {

/// Version of the Nerotypical component (matches the module baseline).
inline constexpr const char *nerotypical_version () noexcept { return "0.1.0"; }

/// Outcome of a type analysis.  Only `WellTyped` authorizes downstream
/// code generation decisions; `TypeError` is a definite (encoding-exact or
/// unification-exact) refutation; `Unknown` means the solver or a budget
/// refused to decide; `InvalidInput` means the constraint system itself was
/// malformed or exceeded a declared cap.
enum class AnalysisStatus : std::uint8_t
{
  WellTyped,
  TypeError,
  Unknown,
  InvalidInput
};

inline const char *analysis_status_name (AnalysisStatus status)
{
  switch (status)
    {
    case AnalysisStatus::WellTyped: return "well_typed";
    case AnalysisStatus::TypeError: return "type_error";
    case AnalysisStatus::Unknown: return "unknown";
    case AnalysisStatus::InvalidInput: return "invalid_input";
    }
  return "invalid_input";
}

/// One contributor to a reported conflict: a single unsatisfiable
/// constraint (set typing), a constructor clash, an occurs-check failure or
/// a violated disequality (structural typing).  Spans identify the source
/// the constraint was generated from; embedders that do not track spans get
/// the default (empty-file 1:1) span.
struct ConflictReport
{
  std::string kind;        // "constraint" | "type_clash" | "occurs_check" | "disequality"
  std::string description;  // human-readable rendering
  SourceSpan span;
  std::string note;
};

/// Observable solving effort and honesty flags.  `localization_incomplete`
/// marks a conflict superset that could not be minimized within the solver
/// call budget; `precision_incomplete` marks must/may sets truncated by the
/// same budget.  Neither flag ever changes a status.
struct AnalysisStats
{
  std::string engine = "none";  // satie engine name, or "euf"
  std::size_t solver_calls = 0;
  std::size_t cnf_variables = 0;          // set typing only
  std::size_t cnf_clauses = 0;             // set typing only
  std::size_t type_terms = 0;              // structural typing only
  std::size_t unification_rounds = 0;     // structural typing only
  bool localization_incomplete = false;
  bool precision_incomplete = false;
};

/// Budgets and knobs.  Every budget is enforced with an honest status/flag:
/// exceeding `max_atoms` is an `InvalidInput`, a zero or exhausted
/// `max_solver_calls` yields `Unknown` (or an incomplete flag when only
/// localization/precision work is curtailed), and structural mode reports
/// `Unknown` when its term or unification-round budgets are exhausted.
struct AnalysisOptions
{
  satie::Engine engine = satie::Engine::CDCL;  // set-typing solver engine
  std::size_t max_atoms = 128;                 // universe atom cap (set typing)
  std::size_t max_solver_calls = 256;          // satie invocation cap (set typing)
  bool precise_typing = false;                 // exact must/may per (variable, atom)
  std::size_t max_type_terms = 4096;           // term cap (structural typing)
  std::size_t max_unification_rounds = 256;    // decomposition round cap (structural)
  bool allow_recursive_types = false;          // occurs-check policy (structural)
};

/// The mode-independent part of every Nerotypical result.
struct BaseAnalysis
{
  AnalysisStatus status = AnalysisStatus::InvalidInput;
  Diagnostics diagnostics;
  std::vector<ConflictReport> conflict;
  AnalysisStats stats;
  bool well_typed () const { return status == AnalysisStatus::WellTyped; }
};

/// Appends a diagnostic and returns it for further customization.
inline Diagnostic &push_diagnostic (BaseAnalysis &base, std::string id, Severity severity,
                                    std::string message, SourceSpan span)
{
  Diagnostic d;
  d.id = std::move (id);
  d.severity = severity;
  d.message = std::move (message);
  d.primary = span;
  base.diagnostics.push_back (std::move (d));
  return base.diagnostics.back ();
}

} // namespace moonstone::nerotypical
