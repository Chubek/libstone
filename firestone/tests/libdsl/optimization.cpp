#include "../../examples/libdsl/reference.hpp"

namespace
{
void
check (bool result)
{
  if (!result)
    throw std::logic_error ("W12 follow-up assertion failed");
}

template <class Error, class Action>
void
reject (Action action)
{
  try
    {
      action ();
    }
  catch (const Error &)
    {
      return;
    }
  throw std::logic_error ("W12 follow-up expected exception was not thrown");
}

dsl::SExpr
arithmetic_syntax (const dsl::SExpr &ast)
{
  if (ast.is_atom)
    throw std::invalid_argument ("expected arithmetic AST");
  if (ast.items.at (0).text == "num")
    return ast.items.at (1);
  std::vector<dsl::SExpr> result{ ast.items.at (0) };
  for (std::size_t i = 1; i < ast.items.size (); ++i)
    result.push_back (arithmetic_syntax (ast.items[i]));
  return dsl::SExpr::list (std::move (result));
}
}

int
main ()
{
  using namespace dsl;
  using namespace dsl::synth;
  const auto arith = reference::arithmetic ();
  reference::ArithmeticState state;
  const auto ast_rule = read_reviewed_optimization_rule ("(rewrite ast-zero (+ ?x (num 0)) ?x)");
  const auto expr_rule = read_reviewed_optimization_rule ("(rewrite add-zero (+ ?x 0) ?x)");
  const auto rules = std::span (&expr_rule, 1);
  std::size_t certified = 0, observed = 0;
  const OptimizationReview review{
    "checked signed arithmetic", "addition of exact zero preserves value and traps",
    [&] (const OptimizationClaim &claim) {
      ++certified;
      // Exact independently reviewable fact: checked + with rhs 0 never
      // overflows and adds no observation except evaluation of x itself.
      // The language is pure: hash-consing cannot collapse side effects.
      const auto &expected = claim.backend == OptimizationBackend::ast_root
                             ? ast_rule : expr_rule;
      return claim.domain == "checked signed arithmetic"
             && claim.rules.size () == 1
             && claim.rules.front ().name == expected.name
             && claim.rules.front ().lhs == expected.lhs
             && claim.rules.front ().rhs == expected.rhs;
    },
    [&] (const OptimizationClaim &claim, const SExpr &candidate) {
      ++observed;
      if (claim.backend == OptimizationBackend::ast_root)
        return arith.compile_expression (arithmetic_syntax (claim.subject)).run (state)
               == arith.compile_expression (arithmetic_syntax (candidate)).run (state);
      return arith.compile_expression (claim.subject).run (state)
             == arith.compile_expression (candidate).run (state);
    }
  };

  const auto ast = sexpr_to_ast (parse_sexpr ("(+ (* (num 6) (num 7)) (num 0))"));
  const auto ast_candidate = reviewed_ast_candidate (ast, ast_rule, review);
  check (ast_candidate && ast_to_sexpr (*ast_candidate) == parse_sexpr ("(* (num 6) (num 7))"));
  check (!reviewed_ast_candidate (*ast_candidate, ast_rule, review));
  auto malformed = leaf<"num"> (1);
  malformed.children ().push_back (leaf<"num"> (2));
  reject<std::invalid_argument> ([&] { reviewed_ast_candidate (malformed, ast_rule, review); });

  const auto shared = parse_sexpr ("(* (+ 6 0) (+ 6 0))");
  const auto sorted = parse_sexpr ("(* 6 6)");
  check (reviewed_graph_reduce (shared, rules, review, 1) == sorted);
  check (reviewed_equality_saturate (shared, rules, review) == sorted);
  check (certified == 4 && observed == 3);

  auto bad_review = review;
  bad_review.certify = {};
  reject<std::invalid_argument> ([&] { reviewed_graph_reduce (shared, rules, bad_review); });
  bad_review = review;
  bad_review.observe = [] (const OptimizationClaim &, const SExpr &) { return false; };
  reject<std::runtime_error> ([&] { reviewed_equality_saturate (shared, rules, bad_review); });
  const auto erasing = read_reviewed_optimization_rule ("(rewrite division (/ ?x 0) ?x)");
  reject<std::invalid_argument> ([&] {
    reviewed_equality_saturate (shared, std::span (&erasing, 1), review);
  });
  reject<std::invalid_argument> ([] {
    read_reviewed_optimization_rule ("(rewrite forged (+ ?x 0) ?y)");
  });
  reject<std::invalid_argument> ([&] {
    reviewed_graph_reduce (parse_sexpr ("(f)"), rules, review);
  });
  reject<std::invalid_argument> ([&] {
    reviewed_equality_saturate (parse_sexpr ("(f 1 2 3 4 5)"), rules, review);
  });
  reject<std::invalid_argument> ([&] {
    reviewed_equality_saturate (shared, std::span (&ast_rule, 1), review);
  });
  const auto before_limits = observed;
  reject<std::runtime_error> ([&] { reviewed_graph_reduce (shared, rules, review, 0); });
  reject<std::runtime_error> ([&] {
    reviewed_equality_saturate (shared, rules, review, { 0, 64, 32 });
  });
  reject<std::runtime_error> ([&] {
    reviewed_equality_saturate (shared, rules, review, { 4, 1, 32 });
  });
  reject<std::runtime_error> ([&] {
    reviewed_equality_saturate (shared, rules, review, { 4, 64, 0 });
  });
  check (observed == before_limits);

  // The terms are pure and total under the stated domain, but the graph's
  // unfolded result is still too big to hand off for observation.
  const auto duplicate = read_reviewed_optimization_rule (
      "(rewrite duplicate (identity ?x) (first ?x ?x ?x ?x))");
  bool expansion_observed = false;
  const OptimizationReview expand{
    "total pure projection terms", "identity and first of equal operands agree",
    [&] (const OptimizationClaim &claim) {
      return claim.rules.size () == 1 && claim.rules[0].lhs == duplicate.lhs
             && claim.rules[0].rhs == duplicate.rhs;
    },
    [&] (const OptimizationClaim &, const SExpr &) { expansion_observed = true; return true; }
  };
  const auto explosive = parse_sexpr ("(identity (identity (identity (identity (identity x)))))");
  reject<std::length_error> ([&] {
    reviewed_graph_reduce (explosive, std::span (&duplicate, 1), expand);
  });
  check (!expansion_observed);
}
