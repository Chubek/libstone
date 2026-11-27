#include "LibDSL.hpp"

namespace
{
using namespace dsl;
using namespace dsl::synth;

void check(bool result)
{
  if (!result)
    throw std::logic_error("W11 test check failed");
}

template <class Callback>
SynthesisFailure fails(SynthesisError expected, Callback callback)
{
  try { callback(); }
  catch (const SynthesisFailure& failure)
    {
      check(failure.code == expected);
      return failure;
    }
  throw std::logic_error("W11 expected a synthesis failure");
}

void attribute_dependencies()
{
  using Engine = AttributeEngine<int, int>;
  Engine engine;
  const AttributeKey env{"P", "env"}, child{"P/0", "env"}, value{"P/0", "value"},
    converted{"P/0", "context"}, total{"P", "total"};
  int reads = 0, calculations = 0;
  engine.declare_text("(attribute P total (depends (P/0 value) (P/0 context)))",
                      [&](const auto& ctx) { ++calculations; return ctx.get(value) + ctx.at(1); });
  engine.declare(env, {}, [&](const auto& ctx) { ++reads; return ctx.state(); });
  engine.declare_text("(attribute P/0 env (depends (P env)) inherited)", [](const auto& ctx)
    {
      check(ctx.role() == AttributeRole::inherited);
      return ctx.at(0) + 2;
    });
  engine.declare(value, {child}, [](const auto& ctx) { return ctx.at(0) + 3; });
  engine.declare(dsl::parse_sexpr("(attribute P/0 context (depends (P/0 env)) contextual)"),
                 [](const auto& ctx)
    {
      check(ctx.role() == AttributeRole::contextual);
      return ctx.at(0) * 2 + ctx.state();
    });
  const std::array requested{total, value, total, env};
  const auto results = engine.evaluate(7, requested);
  check(results.size() == 3 && results.at(total) == 37 && results.at(value) == 12
        && results.at(env) == 7 && reads == 1 && calculations == 1);
  check(engine.evaluate(8, total) == 41 && reads == 2);

  Engine::Session session(engine, 7);
  check(session.evaluate(total) == 37 && reads == 3);
  check(session.evaluate(value) == 12 && reads == 3);
  check(session.evaluate(total) == 37 && calculations == 3);
  session.invalidate(value);
  check(session.evaluate(total) == 37 && reads == 3 && calculations == 4);
  session.invalidate(env);
  check(session.evaluate(total) == 37 && reads == 4 && calculations == 5);
  session.replace_state(8);
  check(session.evaluate(total) == 41 && reads == 5);

  const AttributeKey missing{"X", "unknown"}, broken{"P", "broken"};
  engine.declare(broken, {missing}, [](const auto& ctx) { return ctx.at(0); }, {12, 30, 2});
  const std::array invalid{total, broken};
  const auto error = fails(SynthesisError::missing_attribute,
                           [&] { static_cast<void>(engine.evaluate(0, invalid)); });
  check(error.trace == std::vector<AttributeKey>{broken, missing});
  check(error.diagnostic().span == live::SourceSpan{12, 30, 2} && reads == 5);
  fails(SynthesisError::duplicate_declaration, [&] { engine.declare(env, {}, [](const auto&) { return 0; }); });
  fails(SynthesisError::invalid_declaration, [&] { engine.declare_text("(attribute \"P\" x (depends))", [](const auto&) { return 0; }); });
  fails(SynthesisError::budget, [&] { engine.evaluate(0, total, {1, 2}); });
  const AttributeKey bad_rule{"P", "undeclared"};
  engine.declare(bad_rule, {}, [&](const auto& ctx) { return ctx.get(total); });
  fails(SynthesisError::undeclared_dependency, [&] { engine.evaluate(0, bad_rule); });
}

void circular_attributes()
{
  using Engine = AttributeEngine<int, int>;
  const AttributeKey a{"A", "attr"}, b{"B", "attr"}, c{"C", "final"};
  Engine engine;
  engine.declare(a, {b}, [](const auto& ctx) { return ctx.at(0); });
  engine.declare(b, {a}, [](const auto& ctx) { return ctx.at(0); });
  const auto error = fails(SynthesisError::circular_dependency, [&] { engine.evaluate(0, a); });
  check(error.trace.size() == 2);

  const Engine::FixedPoint finite{0, [](int old, int next) { return std::max(old, next); }, std::equal_to<int>{}};
  Engine fixed;
  std::vector<std::pair<AttributeKey, int>> observed;
  fixed.declare_text("(circular A attr (depends (B attr)))",
      [&](const auto& ctx) { observed.emplace_back(ctx.key(), ctx.at(0)); return std::min(ctx.state(), ctx.at(0) + 1); },
      {}, finite);
  fixed.declare(b, {a}, [&](const auto& ctx) { observed.emplace_back(ctx.key(), ctx.at(0)); return ctx.at(0); }, {}, finite);
  fixed.declare(c, {b}, [](const auto& ctx) { return ctx.at(0) * 2; });
  check(fixed.evaluate(3, c) == 6);
  check(observed.size() >= 4 && observed[0].second == 0 && observed[1].second == 0);
  fails(SynthesisError::nonconvergent, [&] { fixed.evaluate(3, c, {1000, 2}); });
  Engine::Session session(fixed, 3);
  check(session.evaluate(c) == 6);
  const auto before = observed.size();
  check(session.evaluate(c) == 6 && observed.size() == before);
  session.invalidate(a);
  check(session.evaluate(c) == 6 && observed.size() == before * 1 + 14);

  // A long dependency chain is scheduled without recursive graph traversal.
  Engine chain;
  auto prior = AttributeKey{"0", "step"};
  chain.declare(prior, {}, [](const auto&) { return 0; });
  for (int n = 1; n < 1024; ++n)
    {
      const AttributeKey key{std::to_string(n), "step"};
      chain.declare(key, {prior}, [](const auto& ctx) { return 1 + ctx.at(0); });
      prior = key;
    }
  check(chain.evaluate(0, prior) == 1023);
}

void phases_and_scopes()
{
  using Procedures = MetaProcedures<int, int>;
  const auto root = Scope<int>{}.extend({{"x", 5, {1, 2}}});
  const auto caller = root.extend({{"x", 8}, {"delta", 100}});
  auto closure = root.extend({{"x", 13}});
  Procedures p;
  int reads = 0, writes = 0, selectors = 0;
  const Procedures::Analyzer sum{[&](const auto& context)
    {
      ++reads;
      return context.state + context.resolve("x") + context.resolve("delta");
    }};
  p.define_text("(metaproc static analyze lexical (delta))", sum, closure);
  p.define({"dynamic", {"delta"}, sum, ScopePolicy::dynamic, closure});
  p.define_text("(metaproc custom analyze custom (delta))", sum, closure,
                [&](int state, const auto& def, const auto& call) {
                  ++selectors;
                  return def.extend({{"x", state > 0 ? call.require("x") * 2 : def.require("x")}});
                });
  closure = closure.extend({{"x", 20}});
  check(p.analyze(1, caller, {"static", {2}}) == 16);
  check(p.analyze(1, caller, {"dynamic", {2}}) == 11);
  check(p.analyze(1, caller, {"custom", {2}}) == 19 && selectors == 1);
  check(root.find("x")->source == live::SourceSpan{1, 2});
  fails(SynthesisError::missing_binding, [&] { caller.require("absent"); });
  fails(SynthesisError::duplicate_binding, [&] { root.extend({{"x", 1}, {"x", 2}}); });
  fails(SynthesisError::argument_count, [&] { p.analyze(1, caller, {"static", {}}); });

  p.define({"validate", {}, Procedures::Validator{[](const auto& context)
       {
         std::vector<SemanticDiagnostic> reports;
         if (context.state < 0)
           reports.push_back({{live::Severity::error, "negative", "negative state", context.source}, {}});
         return reports;
       }}});
  p.define({"change", {"delta"}, Procedures::Transformer{[&](const auto& context)
       { ++writes; return context.state + context.resolve("delta"); }}});
  const std::array invalid{Procedures::Invocation{"custom", {3}}, Procedures::Invocation{"change", {3}}};
  fails(SynthesisError::wrong_phase, [&] { p.inspect(1, caller, invalid); });
  check(reads == 3 && selectors == 1 && writes == 0);
  fails(SynthesisError::wrong_phase, [&] { p.analyze(1, caller, {"change", {3}}); });
  const std::array validators{Procedures::Invocation{"validate", {}, {3, 4}}};
  check(p.transform(1, caller, {"change", {3}}, validators) == 4);
  const auto failure = fails(SynthesisError::inadmissible_state,
             [&] { p.transform(1, caller, {"change", {-3}}, validators); });
  check(failure.span == live::SourceSpan{3, 4} && writes == 2);
  const std::array inspect{Procedures::Invocation{"static", {2}}, validators[0]};
  const auto result = p.inspect(-2, caller, inspect);
  check(std::get<0>(result[0]) == 13 && std::get<1>(result[1]).size() == 1 && writes == 2);
}

void diagnostic_patterns()
{
  PatternDiagnostics<int> patterns;
  int guards = 0;
  patterns.add_text(R"((diagnostic syntax warning redundant (eq ?x ?x) "same operand" "true"))");
  patterns.add({DiagnosticPhase::semantic, parse_sexpr("(eq ?x ?x)"),
         live::Severity::error, "invalid-type", "bad type", {{"declared here", {1, 2, 1}}},
         std::nullopt, [&](int state, const SExprBindings& bindings)
           { ++guards; return state == 1 && bindings.at("?x") == SExpr::atom("a"); }});
  const auto subject = parse_sexpr("(eq a a)");
  const auto syntax = patterns.syntax(subject, {7, 15, 1});
  check(syntax.size() == 1 && syntax[0].diagnostic.severity == live::Severity::warning
        && syntax[0].fixes[0].span == live::SourceSpan{7, 15, 1}
        && syntax[0].fixes[0].replacement == "true" && guards == 0);
  check(patterns.syntax(parse_sexpr("(eq a b)"), {}).empty());
  check(patterns.semantic(0, subject, {}).empty());
  const auto semantic = patterns.semantic(1, subject, {7, 15, 1});
  check(semantic.size() == 1 && semantic[0].diagnostic.notes[0].span == live::SourceSpan{1, 2, 1}
        && semantic[0].diagnostic.severity == live::Severity::error && guards == 2);
  fails(SynthesisError::budget, [&] { patterns.syntax(subject, {}, 3); });
  fails(SynthesisError::invalid_declaration,
        [&] { patterns.add_text("(diagnostic semantic note missing)"); });
  SExpr deep = SExpr::atom("x");
  for (int i = 0; i < 130; ++i)
    {
      std::vector<SExpr> items;
      items.push_back(std::move(deep));
      deep = SExpr::list(std::move(items));
    }
  fails(SynthesisError::budget, [&] { patterns.syntax(deep, {}); });
}
} // namespace

int main()
{
  attribute_dependencies();
  circular_attributes();
  phases_and_scopes();
  diagnostic_patterns();
}
