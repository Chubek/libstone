// Nerotypical Mode A tests: constraint kinds, exactness differential
// against brute-force enumeration, unsat-core localization, budgets,
// validation, determinism, and the recorded satie `solve_under` repro.

#include <nerotypical/SetTyping.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace moonstone::nerotypical;
using namespace moonstone::nerotypical::settype;

#define CHECK(cond)                                                            \
  do                                                                           \
    {                                                                          \
      if (!(cond))                                                             \
        {                                                                      \
          std::cerr << "FAILED: " #cond " (line " << __LINE__ << ")\n";         \
          return 1;                                                            \
        }                                                                      \
    }                                                                          \
  while (0)

// ---------------------------------------------------------------------------
// Independent brute-force semantics (set masks) used to cross-check the
// SAT encoding: soundness *and* completeness.
// ---------------------------------------------------------------------------
using Mask = std::uint32_t;

static Mask
const_mask (const AtomSet &atoms)
{
  Mask m = 0;
  for (AtomId a : atoms)
    m |= (static_cast<Mask> (1) << a);
  return m;
}

static Mask
term_mask (const TypeTerm &t, const std::vector<Mask> &values)
{
  return t.kind == TypeTerm::Kind::Variable ? values[t.variable] : const_mask (t.atoms);
}

static bool
satisfies_mask (const TypeConstraint &c, const std::vector<Mask> &values, Mask universe)
{
  const Mask a = term_mask (c.a, values);
  const Mask b = term_mask (c.b, values);
  switch (c.kind)
    {
    case TypeConstraint::Kind::Equal: return a == b;
    case TypeConstraint::Kind::Subtype: return (a & ~b) == 0;
    case TypeConstraint::Kind::Union: return term_mask (c.result, values) == (a | b);
    case TypeConstraint::Kind::Intersect: return term_mask (c.result, values) == (a & b);
    case TypeConstraint::Kind::Complement: return term_mask (c.result, values) == (~a & universe);
    case TypeConstraint::Kind::Disjoint: return (a & b) == 0;
    case TypeConstraint::Kind::Inhabited: return a != 0;
    case TypeConstraint::Kind::Empty: return a == 0;
    }
  return false;
}

struct Brute
{
  bool satisfiable = false;
  std::vector<Mask> must;  // per variable
  std::vector<Mask> may;   // per variable
};

static Brute
brute_force (const ConstraintSystem &system, std::size_t atom_count)
{
  const Mask universe = (atom_count >= 32) ? ~Mask (0) : ((Mask (1) << atom_count) - 1);
  const std::size_t per_var = static_cast<std::size_t> (universe) + 1;
  const std::size_t vars = system.variable_count ();
  Brute out;
  out.must.assign (vars, universe);
  out.may.assign (vars, 0);
  std::uint64_t combos = 1;
  for (std::size_t i = 0; i < vars; ++i)
    combos *= per_var;
  std::vector<Mask> values (vars, 0);
  for (std::uint64_t combo = 0; combo < combos; ++combo)
    {
      std::uint64_t rest = combo;
      for (std::size_t v = 0; v < vars; ++v)
        {
          values[v] = static_cast<Mask> (rest % per_var);
          rest /= per_var;
        }
      bool ok = true;
      for (std::size_t k = 0; k < system.constraint_count () && ok; ++k)
        ok = satisfies_mask (system.constraint (k), values, universe);
      if (!ok)
        continue;
      out.satisfiable = true;
      for (std::size_t v = 0; v < vars; ++v)
        {
          out.must[v] &= values[v];
          out.may[v] |= values[v];
        }
    }
  if (!out.satisfiable)
    for (std::size_t v = 0; v < vars; ++v)
      out.must[v] = out.may[v] = 0;
  return out;
}

static Mask
witness_mask (const SetTypingResult &r, std::size_t var, const ConstraintSystem &system)
{
  return const_mask (r.witness.at (var));
}

// ---------------------------------------------------------------------------
// Constraint builders
// ---------------------------------------------------------------------------
static TypeConstraint
equal (TypeTerm a, TypeTerm b, std::string note = {})
{
  TypeConstraint c;
  c.kind = TypeConstraint::Kind::Equal;
  c.a = std::move (a);
  c.b = std::move (b);
  c.note = std::move (note);
  return c;
}

static TypeConstraint
subtype (TypeTerm a, TypeTerm b, std::string note = {})
{
  TypeConstraint c;
  c.kind = TypeConstraint::Kind::Subtype;
  c.a = std::move (a);
  c.b = std::move (b);
  c.note = std::move (note);
  return c;
}

static TypeConstraint
union_of (TypeTerm result, TypeTerm a, TypeTerm b, std::string note = {})
{
  TypeConstraint c;
  c.kind = TypeConstraint::Kind::Union;
  c.result = std::move (result);
  c.a = std::move (a);
  c.b = std::move (b);
  c.note = std::move (note);
  return c;
}

static TypeConstraint
intersect_of (TypeTerm result, TypeTerm a, TypeTerm b, std::string note = {})
{
  TypeConstraint c;
  c.kind = TypeConstraint::Kind::Intersect;
  c.result = std::move (result);
  c.a = std::move (a);
  c.b = std::move (b);
  c.note = std::move (note);
  return c;
}

static TypeConstraint
complement_of (TypeTerm result, TypeTerm a, std::string note = {})
{
  TypeConstraint c;
  c.kind = TypeConstraint::Kind::Complement;
  c.result = std::move (result);
  c.a = std::move (a);
  c.note = std::move (note);
  return c;
}

static TypeConstraint
disjoint (TypeTerm a, TypeTerm b, std::string note = {})
{
  TypeConstraint c;
  c.kind = TypeConstraint::Kind::Disjoint;
  c.a = std::move (a);
  c.b = std::move (b);
  c.note = std::move (note);
  return c;
}

static TypeConstraint
inhabited (TypeTerm a, std::string note = {})
{
  TypeConstraint c;
  c.kind = TypeConstraint::Kind::Inhabited;
  c.a = std::move (a);
  c.note = std::move (note);
  return c;
}

static TypeConstraint
empty_of (TypeTerm a, std::string note = {})
{
  TypeConstraint c;
  c.kind = TypeConstraint::Kind::Empty;
  c.a = std::move (a);
  c.note = std::move (note);
  return c;
}

static std::string
atoms_str (const ConstraintSystem &s, const AtomSet &set)
{
  return render_atoms (s.universe (), set);
}

// Runs one exactness differential: verdict + must/may (precise mode) and
// witness validity must match brute force for every engine.
static int
check_against_brute_force (const ConstraintSystem &system, const char *label)
{
  const Brute expected = brute_force (system, system.universe ().size ());
  const std::size_t vars = system.variable_count ();
  for (const satie::Engine engine : { satie::Engine::CDCL, satie::Engine::DPLL,
                                      satie::Engine::Native })
    {
      AnalysisOptions options;
      options.engine = engine;
      options.precise_typing = true;
      options.max_solver_calls = 4096;
      const SetTypingResult r = system.solve (options);
      if (expected.satisfiable)
        {
          CHECK (r.base.status == AnalysisStatus::WellTyped);
          // The witness must itself satisfy every constraint.
          std::vector<Mask> values (vars);
          for (std::size_t v = 0; v < vars; ++v)
            values[v] = witness_mask (r, v, system);
          for (std::size_t k = 0; k < system.constraint_count (); ++k)
            CHECK (satisfies_mask (system.constraint (k), values,
                                   (Mask (1) << system.universe ().size ()) - 1));
          for (std::size_t v = 0; v < vars; ++v)
            {
              CHECK (const_mask (r.must_atoms.at (v)) == expected.must[v]);
              CHECK (const_mask (r.may_atoms.at (v)) == expected.may[v]);
              CHECK ((const_mask (r.witness.at (v)) & ~expected.may[v]) == 0);
            }
        }
      else
        CHECK (r.base.status == AnalysisStatus::TypeError);
    }
  std::cout << "  differential ok: " << label << "\n";
  return 0;
}

int
main ()
{
  std::cout << "nerotypical set-typing tests\n";

  // --- universe and term rendering -------------------------------------
  {
    TypeUniverse u;
    const AtomId a = u.intern ("int");
    CHECK (u.find ("int") && *u.find ("int") == a);
    CHECK (u.intern ("int") == a);
    CHECK (u.size () == 1);
    CHECK (render_atoms (u, { a }) == "{int}");
    CHECK (render_atoms (u, {}) == "{}");
    bool threw = false;
    try
      {
        u.name (99);
      }
    catch (const std::out_of_range &)
      {
        threw = true;
      }
    CHECK (threw);
  }
  std::cout << "  universe ok\n";

  // --- constraint kind semantics, SAT and UNSAT shapes ------------------
  {
    ConstraintSystem s;
    const AtomId i = s.atom ("int");
    const AtomId f = s.atom ("float");
    const AtomId b = s.atom ("bool");
    s.atom ("string");  // a fourth, unused atom
    s.atom ("unit");

    const TypeVarId x = s.fresh_variable ("x");
    const TypeVarId y = s.fresh_variable ("y");
    const TypeVarId z = s.fresh_variable ("z");

    // Equal + Subtype chain is SAT.
    CHECK (!s.add (equal (variable_term (x), constant_term ({ i }))));
    CHECK (!s.add (subtype (variable_term (x), variable_term (y))));
    CHECK (!s.add (inhabited (variable_term (y))));
    SetTypingResult r = s.solve ();
    CHECK (r.base.status == AnalysisStatus::WellTyped);
    CHECK (r.witness.size () == 3);
    CHECK (atoms_str (s, r.witness[x]) == "{int}");
    // b(y,int) is forced by the subtype chain; other y bits are the
    // solver's choice, so only assert the forced part.
    CHECK (std::find (r.witness[y].begin (), r.witness[y].end (), i) != r.witness[y].end ());
    CHECK (!r.base.stats.localization_incomplete);

    // Exact literals clash: x = {int} and x = {bool}.
    ConstraintSystem clash;
    const AtomId ci = clash.atom ("int");
    const AtomId cb = clash.atom ("bool");
    const TypeVarId t = clash.fresh_variable ("t");
    CHECK (!clash.add (equal (variable_term (t), constant_term ({ ci }))));
    CHECK (!clash.add (equal (variable_term (t), constant_term ({ cb }))));
    r = clash.solve ();
    CHECK (r.base.status == AnalysisStatus::TypeError);
    CHECK (r.base.conflict.size () == 2);
    CHECK (r.base.diagnostics.size () == 1);
    CHECK (r.base.diagnostics[0].id == "nerotypical.type.conflict");

    // Union / Intersect / Complement / Disjoint / Empty semantics.
    ConstraintSystem ops;
    const AtomId oi = ops.atom ("int");
    const AtomId of = ops.atom ("float");
    const AtomId ob = ops.atom ("bool");
    const TypeVarId u1 = ops.fresh_variable ("u1");
    const TypeVarId u2 = ops.fresh_variable ("u2");
    const TypeVarId n1 = ops.fresh_variable ("n1");
    CHECK (!ops.add (union_of (variable_term (u1), constant_term ({ oi }),
                               constant_term ({ of }))));
    CHECK (!ops.add (intersect_of (variable_term (n1), variable_term (u1),
                                   constant_term ({ oi, ob }))));
    CHECK (!ops.add (complement_of (variable_term (u2), variable_term (n1))));
    r = ops.solve ();
    CHECK (r.base.status == AnalysisStatus::WellTyped);
    CHECK (atoms_str (ops, r.witness[u1]) == "{int, float}");
    CHECK (atoms_str (ops, r.witness[n1]) == "{int}");
    CHECK (atoms_str (ops, r.witness[u2]) == "{float, bool}");

    // Disjoint + inhabited exact literals: x={int}, y={float}, disjoint SAT.
    ConstraintSystem d;
    const AtomId di = d.atom ("int");
    const AtomId df = d.atom ("float");
    const TypeVarId dx = d.fresh_variable ();
    const TypeVarId dy = d.fresh_variable ();
    CHECK (!d.add (equal (variable_term (dx), constant_term ({ di }))));
    CHECK (!d.add (equal (variable_term (dy), constant_term ({ df }))));
    CHECK (!d.add (disjoint (variable_term (dx), variable_term (dy))));
    CHECK (d.solve ().base.status == AnalysisStatus::WellTyped);
    // Disjoint with identical exact literals is UNSAT.
    CHECK (!d.add (equal (variable_term (dy), constant_term ({ di }))));
    CHECK (d.solve ().base.status == AnalysisStatus::TypeError);

    // Empty: x = {} satisfies Subtype vacuously, but not Equal to {int}.
    ConstraintSystem e;
    const AtomId ei = e.atom ("int");
    const TypeVarId ex = e.fresh_variable ();
    CHECK (!e.add (empty_of (variable_term (ex))));
    CHECK (!e.add (subtype (variable_term (ex), constant_term ({ ei }))));
    CHECK (e.solve ().base.status == AnalysisStatus::WellTyped);
    CHECK (!e.add (equal (variable_term (ex), constant_term ({ ei }))));
    CHECK (e.solve ().base.status == AnalysisStatus::TypeError);

    // Inhabited + Empty constant contradictions are immediate.
    ConstraintSystem ic;
    ic.atom ("int");
    CHECK (!ic.add (inhabited (constant_term ({}))));
    CHECK (ic.solve ().base.status == AnalysisStatus::TypeError);
    ConstraintSystem ec;
    const AtomId eci = ec.atom ("int");
    CHECK (!ec.add (empty_of (constant_term ({ eci }))));
    CHECK (ec.solve ().base.status == AnalysisStatus::TypeError);
  }
  std::cout << "  constraint kinds ok\n";

  // --- unsat core: a contradictory trio where every pair is SAT ----------
  {
    for (const satie::Engine engine : { satie::Engine::CDCL, satie::Engine::DPLL,
                                        satie::Engine::Native })
      {
        ConstraintSystem s;
        const AtomId i = s.atom ("int");
        const AtomId f = s.atom ("float");
        const AtomId b = s.atom ("bool");
        const TypeVarId x = s.fresh_variable ("x");
        const TypeVarId y = s.fresh_variable ("y");
        CHECK (!s.add (equal (variable_term (x), constant_term ({ i, f }), "x is numeric")));
        CHECK (!s.add (subtype (variable_term (x), variable_term (y), "x flows into y")));
        CHECK (!s.add (equal (variable_term (y), constant_term ({ b }), "y must be bool")));
        AnalysisOptions options;
        options.engine = engine;
        const SetTypingResult r = s.solve (options);
        CHECK (r.base.status == AnalysisStatus::TypeError);
        CHECK (!r.base.stats.localization_incomplete);
        CHECK (r.base.conflict.size () == 3);
        CHECK (r.base.conflict[0].description == "x = {int, float}");
        CHECK (r.base.conflict[0].note == "x is numeric");

        // Every strict pair is satisfiable: the core is genuinely minimal.
        for (std::size_t drop = 0; drop < 3; ++drop)
          {
            ConstraintSystem pair;
            const AtomId pi = pair.atom ("int");
            const AtomId pf = pair.atom ("float");
            const AtomId pb = pair.atom ("bool");
            const TypeVarId px = pair.fresh_variable ("x");
            const TypeVarId py = pair.fresh_variable ("y");
            TypeConstraint c1 =
                equal (variable_term (px), constant_term ({ pi, pf }), "x is numeric");
            TypeConstraint c2 = subtype (variable_term (px), variable_term (py), "x flows into y");
            TypeConstraint c3 = equal (variable_term (py), constant_term ({ pb }), "y must be bool");
            if (drop != 0)
              CHECK (!pair.add (c1));
            if (drop != 1)
              CHECK (!pair.add (c2));
            if (drop != 2)
              CHECK (!pair.add (c3));
            CHECK (pair.solve (options).base.status == AnalysisStatus::WellTyped);
          }
      }
  }
  std::cout << "  unsat core ok (all engines)\n";

  // --- exactness differential vs brute force ------------------------------
  {
    // System 1: 2 vars, 3 atoms — partial determination.
    ConstraintSystem s1;
    const AtomId a1 = s1.atom ("a");
    s1.atom ("b");
    s1.atom ("c");
    const TypeVarId x = s1.fresh_variable ("x");
    const TypeVarId y = s1.fresh_variable ("y");
    CHECK (!s1.add (subtype (variable_term (x), variable_term (y))));
    CHECK (!s1.add (inhabited (variable_term (x))));
    CHECK (!s1.add (disjoint (variable_term (x), constant_term ({ a1 }))));
    if (int rc = check_against_brute_force (s1, "2 vars / 3 atoms"))
      return rc;

    // System 2: 4 vars, 2 atoms — union/intersect/complement mix.
    ConstraintSystem s2;
    s2.atom ("p");
    s2.atom ("q");
    const TypeVarId x2 = s2.fresh_variable ("x");
    const TypeVarId y2 = s2.fresh_variable ("y");
    const TypeVarId z2 = s2.fresh_variable ("z");
    const TypeVarId w2 = s2.fresh_variable ("w");
    CHECK (!s2.add (union_of (variable_term (z2), variable_term (x2), variable_term (y2))));
    CHECK (!s2.add (subtype (variable_term (y2), variable_term (x2))));
    CHECK (!s2.add (intersect_of (variable_term (w2), variable_term (z2), variable_term (x2))));
    CHECK (!s2.add (inhabited (variable_term (w2))));
    if (int rc = check_against_brute_force (s2, "4 vars / 2 atoms"))
      return rc;

    // System 3: unsatisfiable mix — verdict must agree with brute force.
    ConstraintSystem s3;
    s3.atom ("p");
    s3.atom ("q");
    const TypeVarId x3 = s3.fresh_variable ("x");
    const TypeVarId z3 = s3.fresh_variable ("z");
    CHECK (!s3.add (union_of (variable_term (z3), variable_term (x3),
                             constant_term ({ AtomId (0) }))));
    CHECK (!s3.add (empty_of (variable_term (z3))));
    CHECK (!s3.add (equal (variable_term (x3), constant_term ({ AtomId (1) }))));
    if (int rc = check_against_brute_force (s3, "unsatisfiable union"))
      return rc;
  }
  std::cout << "  brute-force differential ok\n";

  // --- budgets and honest degradation -------------------------------------
  {
    // Zero solver budget: Unknown, never well-typed.
    ConstraintSystem s;
    const AtomId i = s.atom ("int");
    const TypeVarId x = s.fresh_variable ();
    CHECK (!s.add (equal (variable_term (x), constant_term ({ i }))));
    AnalysisOptions zero;
    zero.max_solver_calls = 0;
    const SetTypingResult r = s.solve (zero);
    CHECK (r.base.status == AnalysisStatus::Unknown);
    CHECK (!r.base.diagnostics.empty ());

    // Precision budget: verdict stands, precision flagged incomplete.
    // (Two vars over two atoms generate more queries than the budget
    // allows, so the pass must truncate.)
    ConstraintSystem two;
    two.atom ("a");
    two.atom ("b");
    const TypeVarId t1 = two.fresh_variable ();
    const TypeVarId t2 = two.fresh_variable ();
    CHECK (!two.add (subtype (variable_term (t1), variable_term (t2))));
    CHECK (!two.add (inhabited (variable_term (t1))));
    AnalysisOptions tight;
    tight.precise_typing = true;
    tight.max_solver_calls = 2;  // main solve + one query
    const SetTypingResult p = two.solve (tight);
    CHECK (p.base.status == AnalysisStatus::WellTyped);
    CHECK (p.base.stats.precision_incomplete);
    CHECK (!p.base.diagnostics.empty ());

    // Localization budget: TypeError stands, localization flagged.
    ConstraintSystem u;
    const AtomId ui = u.atom ("int");
    const AtomId ub = u.atom ("bool");
    const TypeVarId t = u.fresh_variable ();
    CHECK (!u.add (equal (variable_term (t), constant_term ({ ui }))));
    CHECK (!u.add (equal (variable_term (t), constant_term ({ ub }))));
    AnalysisOptions loc;
    loc.max_solver_calls = 1;  // only the main solve runs
    const SetTypingResult lr = u.solve (loc);
    CHECK (lr.base.status == AnalysisStatus::TypeError);
    CHECK (lr.base.stats.localization_incomplete);
    CHECK (lr.base.conflict.size () == 2);  // unminimized superset

    // Atom cap: a universe above the configured cap is rejected.
    ConstraintSystem capped_system;
    capped_system.atom ("int");
    capped_system.atom ("float");
    capped_system.atom ("bool");
    const TypeVarId capped_var = capped_system.fresh_variable ();
    CHECK (!capped_system.add (inhabited (variable_term (capped_var))));
    AnalysisOptions atoms_cap;
    atoms_cap.max_atoms = 2;
    const SetTypingResult capped = capped_system.solve (atoms_cap);
    CHECK (capped.base.status == AnalysisStatus::InvalidInput);
    CHECK (capped.base.diagnostics[0].id == "nerotypical.invalid_input");

    // Empty universe.
    ConstraintSystem bare;
    CHECK (bare.solve ().base.status == AnalysisStatus::InvalidInput);
  }
  std::cout << "  budgets ok\n";

  // --- add() validation ----------------------------------------------------
  {
    ConstraintSystem s;
    s.atom ("int");
    const TypeVarId x = s.fresh_variable ("x");
    TypeConstraint bad_var = equal (variable_term (x), variable_term (42));
    const auto err1 = s.add (std::move (bad_var));
    CHECK (err1 && err1->find ("type variable") != std::string::npos);
    TypeConstraint bad_atom = equal (variable_term (x), constant_term ({ 7 }));
    const auto err2 = s.add (std::move (bad_atom));
    CHECK (err2 && err2->find ("atom") != std::string::npos);
    CHECK (s.constraint_count () == 0);
  }
  std::cout << "  validation ok\n";

  // --- describe / render ---------------------------------------------------
  {
    ConstraintSystem s;
    const AtomId i = s.atom ("int");
    const AtomId f = s.atom ("float");
    const TypeVarId x = s.fresh_variable ("x");
    const TypeVarId y = s.fresh_variable ("y");
    const TypeVarId z = s.fresh_variable ("z");
    TypeConstraint c = union_of (variable_term (z), variable_term (x), variable_term (y));
    CHECK (s.describe (c) == "z = x | y");
    TypeConstraint e = equal (variable_term (x), constant_term ({ i, f }));
    CHECK (s.describe (e) == "x = {int, float}");
    TypeConstraint sub = subtype (variable_term (x), variable_term (y));
    CHECK (s.describe (sub) == "x <= y");
    TypeConstraint dis = disjoint (variable_term (x), variable_term (y));
    CHECK (s.describe (dis) == "x & y is empty");
    TypeConstraint inh = inhabited (variable_term (x));
    CHECK (s.describe (inh) == "x is inhabited");
    TypeConstraint emp = empty_of (variable_term (x));
    CHECK (s.describe (emp) == "x is empty");
    TypeConstraint cmp = complement_of (variable_term (y), variable_term (x));
    CHECK (s.describe (cmp) == "y = ~x");
    TypeConstraint inter = intersect_of (variable_term (y), variable_term (x),
                                          variable_term (z));
    CHECK (s.describe (inter) == "y = x & z");
  }
  std::cout << "  describe ok\n";

  // --- determinism ---------------------------------------------------------
  {
    ConstraintSystem s;
    const AtomId i = s.atom ("int");
    const AtomId f = s.atom ("float");
    const AtomId b = s.atom ("bool");
    const TypeVarId x = s.fresh_variable ("x");
    const TypeVarId y = s.fresh_variable ("y");
    const TypeVarId z = s.fresh_variable ("z");
    CHECK (!s.add (union_of (variable_term (z), variable_term (x), variable_term (y))));
    CHECK (!s.add (subtype (variable_term (x), constant_term ({ i, f }))));
    CHECK (!s.add (inhabited (variable_term (y))));
    const ConstraintSystem::Encoding e1 = s.encode ();
    const ConstraintSystem::Encoding e2 = s.encode ();
    CHECK (to_dimacs_string (e1.cnf) == to_dimacs_string (e2.cnf));
    CHECK (e1.bits == e2.bits);
    AnalysisOptions options;
    options.precise_typing = true;
    options.max_solver_calls = 4096;
    const SetTypingResult r1 = s.solve (options);
    const SetTypingResult r2 = s.solve (options);
    CHECK (r1.base.status == r2.base.status);
    CHECK (r1.witness == r2.witness);
    CHECK (r1.must_atoms == r2.must_atoms);
    CHECK (r1.may_atoms == r2.may_atoms);
    CHECK (r1.base.stats.cnf_variables == r2.base.stats.cnf_variables);
  }
  std::cout << "  determinism ok\n";

  // --- recorded satie upstream issue ---------------------------------------
  // The vendored satie CDCL `solve_under` (assumption interface) hangs on
  // the trivially unsatisfiable selector-guarded formula below; plain
  // `solve_cdcl` decides it correctly.  Nerotypical therefore avoids
  // `solve_under` entirely and localizes errors by deletion-based core
  // minimization.  This test pins the *shape* that used to reach the
  // hanging path (an exact literal constrained to a disjoint upper bound):
  //
  //   p cnf 14 17
  //   1 -10 0   -2 -10 0   -3 -10 0   -4 -11 0   -5 -11 0   6 -11 0
  //   -3 -12 0  -6 -13 0   -1 7 -14 0 -4 7 -14 0  1 4 -7 -14 0
  //   -2 8 -14 0 -5 8 -14 0 2 5 -8 -14 0
  //   -3 9 -14 0 -6 9 -14 0 3 6 -9 -14 0
  //   (selectors 10..14; assumptions {10,11,12,13,14} hang solve_under)
  {
    ConstraintSystem s;
    const AtomId i = s.atom ("int");
    const AtomId f = s.atom ("float");
    const AtomId str = s.atom ("string");
    const TypeVarId t1 = s.fresh_variable ("t1");
    const TypeVarId t2 = s.fresh_variable ("t2");
    const TypeVarId res = s.fresh_variable ("res");
    CHECK (!s.add (equal (variable_term (t1), constant_term ({ i }), "integer literal")));
    CHECK (!s.add (equal (variable_term (t2), constant_term ({ str }), "string literal")));
    CHECK (!s.add (subtype (variable_term (t1), constant_term ({ i, f }),
                            "arithmetic operand must be numeric")));
    CHECK (!s.add (subtype (variable_term (t2), constant_term ({ i, f }),
                            "arithmetic operand must be numeric")));
    CHECK (!s.add (union_of (variable_term (res), variable_term (t1), variable_term (t2),
                            "arithmetic result")));
    const SetTypingResult res_r = s.solve ();
    CHECK (res_r.base.status == AnalysisStatus::TypeError);
    CHECK (res_r.base.conflict.size () == 2);
    CHECK (res_r.base.conflict[0].description == "t2 = {string}");
    CHECK (res_r.base.conflict[1].description == "t2 <= {int, float}");
  }
  std::cout << "  recorded satie repro shape ok\n";

  std::cout << "set_typing: all tests passed\n";
  return 0;
}
