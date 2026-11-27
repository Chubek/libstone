#include "LibDSL.hpp"

namespace {
using namespace dsl;
using namespace dsl::synth;
struct State { int value; };
using Linked = LinkedSpecification<State, int>;

void check(bool okay)
{
  if (!okay)
    throw std::logic_error("link test failed");
}

template <class Run>
void fails(SynthesisError expected, Run run)
{
  try { run(); }
  catch (const SynthesisFailure &error)
    {
      check(error.code == expected);
      return;
    }
  throw std::logic_error("missing link failure");
}

template <class Run>
void invalid(Run run)
{
  try { run(); }
  catch (const std::invalid_argument &) { return; }
  throw std::logic_error("missing arity failure");
}
}

int main()
{
  int factory_calls = 0;
  int rule_calls = 0;
  Linked::TrustedCallbacks host;
  host.executable
      .on_atom([] (auto &b, const SExpr &s)
        {
          if (!s.quoted && s.text == "value")
            return b.expression([] (auto &f) { return f.state().value; });
          throw std::invalid_argument("unknown atom");
        })
      .on_expression("+", [&] (auto &b, const SExpr &s)
        {
          ++factory_calls;
          auto x = b.expression(s.items.at(1));
          auto y = b.expression(s.items.at(2));
          return b.expression([=] (auto &f) { return f.evaluate(x) + f.evaluate(y); });
        })
      .on_statement("set", [] (auto &b, const SExpr &s)
        {
          auto x = b.expression(s.items.at(1));
          return b.statement([=] (auto &f) { f.state().value = f.evaluate(x); });
        })
      .on_expression("literal", [] (auto &b, const SExpr &)
        {
          // Quoted data is opaque to object expansion and is not dispatched.
          return b.expression([] (auto &) { return 7; });
        });
  host.attribute("seed", [&] (const auto &c) { ++rule_calls; return c.state().value; });
  host.attribute("double", [&] (const auto &c)
    {
      ++rule_calls;
      check(c.role() == AttributeRole::contextual);
      return 2 * c.get(AttributeKey{"leaf", "value"});
    });
  fails(SynthesisError::duplicate_binding, [&] {
    host.attribute("seed", [] (const auto &) { return 0; });
  });

  const auto text = Specification::from_text(R"(
    (language Link
      (form + 2) (form set 1)
      (macro (twice $x) (+ $x $x))
      (expression read (+ value value))
      (expression data (literal (quote (+))))
      (statement read (set (twice value)))
      (attribute result (double (attribute root value (depends (leaf value)) contextual)))
      (attribute base (seed (attribute leaf value (depends)))))
  )");
  const SyntaxOperand value("value");
  const auto native = SpecBuilder("Link").form("+", 2).form("set", 1)
    .macro(parse_sexpr("(twice $x)"), parse_sexpr("(+ $x $x)"))
    .expression("read", value + value)
    .declare(DeclarationKind::expression, "data", parse_sexpr("(literal (quote (+)))"))
    .declare(DeclarationKind::statement, "read", parse_sexpr("(set (twice value))"))
    .declare(DeclarationKind::attribute, "result",
             parse_sexpr("(double (attribute root value (depends (leaf value)) contextual))"))
    .declare(DeclarationKind::attribute, "base",
             parse_sexpr("(seed (attribute leaf value (depends)))")).build();
  check(native.to_sexpr() == text.to_sexpr());
  for (const auto *spec : { &text, &native })
    {
      const auto calls = rule_calls;
      const Linked linked(*spec, host);
      check(rule_calls == calls);
      State state{3};
      check(linked.expression("read").run(state) == 6);
      check(linked.expression("data").run(state) == 7);
      linked.statement("read").run(state);
      check(state.value == 6);
      check(linked.attributes().evaluate(state, AttributeKey{"root", "value"}) == 12);
      check(rule_calls == calls + 2);
    }

  const auto before = factory_calls;
  invalid([&] {
    Linked(Specification::from_text(
      "(language Bad (form + 2) (macro (bad) (+ value)) (statement bad (set (bad))))"), host);
  });
  check(factory_calls == before);
  fails(SynthesisError::missing_binding, [&] {
    Linked(Specification::from_text(
      "(language Bad (attribute a (unknown (attribute n a (depends)))))"), host);
  });
  fails(SynthesisError::invalid_declaration, [&] {
    Linked(Specification::from_text(
      "(language Bad (attribute a (seed (circular n a (depends (n a))))))"), host);
  });
  fails(SynthesisError::missing_attribute, [&] {
    Linked(Specification::from_text(
      "(language Bad (attribute a (seed (attribute n a (depends (n b))))))"), host);
  });
  fails(SynthesisError::duplicate_declaration, [&] {
    Linked(Specification::from_text(
      "(language Bad (attribute a (seed (attribute n a (depends))))"
      " (attribute b (seed (attribute n a (depends)))))"), host);
  });
  fails(SynthesisError::invalid_declaration, [&] {
    Linked(Specification::from_text(
      "(language Bad (attribute a (seed (attribute n a (depends (n b) (n b))))))"), host);
  });

  host.attribute("climb", [] (const auto &c)
    { return std::min(c.state().value, c.get(c.key()) + 1); },
    Linked::Attributes::FixedPoint{0, [](int a, int b) { return std::max(a, b); },
                                   std::equal_to<int>{}});
  const Linked recursive(Specification::from_text(
    "(language Loop (attribute loop (climb (circular n a (depends (n a))))))"), host);
  check(recursive.attributes().evaluate(State{4}, AttributeKey{"n", "a"}) == 4);
}
