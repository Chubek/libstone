#include "novostone/ActionSemantics/ActionSemantics.hpp"
#include "novostone/Datalog/Datalog.hpp"
#include <future>
#include <iostream>
#include <string>
#include <vector>

using namespace novostone::action;
namespace {
void check(bool b, const char* why) { if (!b) throw std::runtime_error(why); }
void rejected(const std::string& source, const std::string& reason) {
  try { (void)parse(source, "bad.acsem"); }
  catch (const ParseError& e) {
    check(e.source == "bad.acsem" && e.line > 0 && e.column > 0 && e.offset <= source.size(), "bad diagnostic location");
    check(std::string(e.what()).find(reason) != std::string::npos, e.what()); return;
  }
  throw std::runtime_error("accepted invalid input: " + source);
}
std::string spec(const std::string& rule) {
  return "language Test { start E; syntax E = N(n: integer) | V(v: identifier); rule N = " + rule + "; rule V = give bound(v); }";
}
void extended_frontend() {
  auto language = parse(spec("give n + 1 * 2"));
  check(language.default_dialect() == Dialect::One, "legacy specs default to dialect 1");
  Options forced;
  forced.dialect = Dialect::Two;
  for (auto t : {Target::C, Target::Python, Target::Rust, Target::Pascal, Target::Latex}) {
    auto output = generate(language, t, forced);
    check(output.find("@TABLES@") == std::string::npos, "unsubstituted tables");
    check(output.find("acsem dialect 2; source FNV-1a-64") != std::string::npos, "missing dialect 2 provenance");
    check(output == generate(parse(spec("give n + 1 * 2")), t, forced), "nondeterministic generation");
    Options one;
    one.dialect = Dialect::One;
    auto legacy = generate(language, t);
    check(legacy == generate(language, t, one), "dialect routing mismatch");
    check(legacy.find("acsem dialect 1;") != std::string::npos, "legacy stamp lost");
    if (t != Target::Latex) {
      auto pool_name = t == Target::Pascal ? "Pool:" : "POOL";
      check(legacy.find(pool_name) != std::string::npos && output.find(pool_name) != std::string::npos,
            "dialects must share the canonical instruction format");
    }
  }
  auto tex = generate(parse("language A_b { start E; syntax E = N_x(a_b: integer); rule N_x = give a_b < 42; }"),
                      Target::Latex, forced);
  check(tex.find("A\\_b") != std::string::npos, "LaTeX escaping");
  check(tex.find("N\\_x") != std::string::npos, "extended semantic equations");
  Options wide;
  wide.dialect = Dialect::Two;
  wide.steps = 200000;
  auto stepped = generate(language, Target::C, wide);
  check(stepped.find("STEPS_MAX = 200000") != stepped.npos, "budget override");
  check(stepped == generate(parse(spec("give n + 1 * 2")), Target::C, wide), "override determinism");
  auto closure_text = std::string("language F { start E; syntax E = N(n: integer) | Make() | Call(f: E, a: E);") +
    " rule N = give n; rule Make = give closure (give 7);" +
    " rule Call = (evaluate(f) and then evaluate(a)) then apply given(1) to (given(2)); }";
  auto functions = parse(closure_text, "functions.acsem");
  check(functions.default_dialect() == Dialect::Two, "closure specs need dialect 2");
  check(functions.name() == "F", "extended name");
  auto out = generate(functions, Target::C);
  check(out.find("acsem dialect 2;") != std::string::npos, "auto-routed dialect 2");
  check(out.find("POOL") != std::string::npos, "missing operand pool");
  check(out == generate(functions, Target::C), "nondeterministic extended generation");
  check(generate(functions, Target::Latex).find("closure") != std::string::npos, "LaTeX closure equations");
  bool refused = false;
  try { Options one; one.dialect = Dialect::One; (void)generate(functions, Target::C, one); }
  catch (const std::invalid_argument& e) { refused = std::string(e.what()).find("dialect 2") != std::string::npos; }
  check(refused, "dialect 1 capability error");
  rejected("language F { start E; syntax E = N(); rule N = letrec \"x\" = closure complete, \"x\" = closure fail in complete; }",
           "duplicate letrec");
  std::string arguments = "1";
  for (int j = 1; j < 65; ++j) arguments += ", 1";
  rejected("language F { start E; syntax E = N(); rule N = apply 1 to (" + arguments + "); }", "64 arguments");
  for (int bad : {-1, 65537}) {
    Options budgets; budgets.dialect = Dialect::Two; budgets.heap = bad;
    bool rejected_budget = false;
    try { (void)generate(language, Target::C, budgets); }
    catch (const std::invalid_argument&) { rejected_budget = true; }
    check(rejected_budget, "invalid library heap budget accepted");
  }
  Options legacy_budget; legacy_budget.steps = 1;
  bool rejected_budget = false;
  try { (void)generate(language, Target::C, legacy_budget); }
  catch (const std::invalid_argument&) { rejected_budget = true; }
  check(rejected_budget, "legacy budget silently ignored");
  auto mixed = parse("language M { start E; syntax E = N(); rule N = generator (callcc given(1)); }");
  for (auto t : {Target::C, Target::Python, Target::Rust, Target::Pascal, Target::Latex}) {
    bool rejected_combination = false;
    try { (void)generate(mixed, t); }
    catch (const std::invalid_argument& e) { rejected_combination = std::string(e.what()).find("capability error") != std::string::npos; }
    check(rejected_combination, "unsafe continuation/generator composition accepted");
  }
  const std::string located = "language M { start E;\nsyntax E = N();\nrule N = closure (\ngive unknown);\n}";
  try { (void)parse(located, "located.acsem"); check(false, "unknown closure field accepted"); }
  catch (const ParseError& e) { check(e.line == 4, "extended diagnostic lost source location"); }
  rejected(spec("give bound(contents(bound(\"x\")))"), "type");
  auto tagged = parse("language T { start E; syntax E = Load() | Store();"
                      " rule Load = give bound(contents(bound(\"r\")));"
                      " rule Store = put \"x\" in bound(\"r\"); }");
  check(tagged.default_dialect() == Dialect::Two, "tagged cells must be detected before lowering earlier rules");
  const std::string adt = "language P { start E; syntax E = N();"
    " data List = Nil() | Cons(integer, List); data Other = Other(datum); rule N = ";
  auto pattern_spec = [&](std::string_view body) { return adt + std::string(body) + "; }"; };
  auto data = parse(pattern_spec("construct Nil() then match given(1) of List with {"
                                 " Nil() => give 0, Cons(x, _) => give bound(\"x\") }"));
  check(data.default_dialect() == Dialect::Two, "data must select extended mode");
  check(generate(data, Target::Latex).find("Runtime data sorts") != std::string::npos, "missing data sort equations");
  rejected(pattern_spec("construct Missing()"), "unknown data constructor");
  rejected(pattern_spec("construct Cons(0)"), "wrong constructor arity");
  rejected(pattern_spec("construct Cons(\"token\", given(1))"), "incompatible field type");
  rejected(pattern_spec("match given(1) with { Cons(x) => fail }"), "wrong pattern arity");
  rejected(pattern_spec("match given(1) with { Cons(x, x) => fail }"), "duplicate pattern binding");
  rejected(pattern_spec("match given(1) with { Cons(x, _) | Nil() => fail }"), "or-pattern alternatives must bind the same names");
  rejected(pattern_spec("match given(1) with { Cons(x, _) as x => fail }"), "duplicate pattern binding");
  rejected(pattern_spec("match given(1) with { Cons(\"token\", _) => fail }"), "incompatible token pattern");
  rejected(pattern_spec("match given(1) with { Cons(x, _) => fail, Cons(1, Nil()) => complete }"), "unreachable match arm");
  rejected(pattern_spec("match given(1) with { _ => complete, Nil() => fail }"), "unreachable match arm");
  rejected(pattern_spec("match given(1) of List with { Nil() => complete }"), "exhaustiveness not established for Cons");
  rejected(pattern_spec("match given(1) of List with { Other(_) => complete, _ => fail }"), "incompatible data constructor pattern");
  rejected("language P { start E; syntax E = N(); data Loop = Loop(Loop); rule N = complete; }", "uninhabited data sort");
  rejected("language P { start E; syntax E = N(); data T = A(Missing); rule N = complete; }", "unknown data sort");
  rejected("language P { start E; syntax E = N(); data T = A() | A(integer); rule N = complete; }", "duplicate data constructor");
}
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--extended") {
      extended_frontend();
      std::cout << "ActionSemantics extended frontend checks passed\n";
      return 0;
    }
    check(argc == 1, "unexpected test argument");
    auto m = parse(spec("give n")); check(m.name() == "Test", "name");
    for (auto t : {Target::C, Target::Python, Target::Rust, Target::Pascal, Target::Latex}) {
      auto s = generate(m, t);
      check(s == generate(parse(spec("give n")), t), "nondeterministic generation");
      check(s.find("@TABLES@") == std::string::npos && s.find("source FNV-1a-64") != std::string::npos, "missing tables/provenance");
    }
    check(generate(parse("language A_b { start E; syntax E = N_x(); rule N_x = give 42; }"), Target::Latex).find("A\\_b") != std::string::npos, "LaTeX escaping");
    rejected(spec("give missing"), "unknown field");
    rejected(spec("evaluate(n)"), "syntax field");
    rejected(spec("give 2147483648"), "32-bit");
    rejected(spec("give -2147483649"), "32-bit");
    rejected(spec("give given(0)"), "1..64");
    rejected(spec("give given(65)"), "1..64");
    rejected(spec("give bound(1)"), "type");
    rejected(spec("give contents(0)"), "type");
    rejected(spec("bind n to 1"), "type");
    rejected(spec("give \"x\" + 1"), "type");
    rejected(spec("give 1 junk"), "syntax");
    rejected(spec("give \"x\\\"; malicious\""), "syntax");
    rejected(spec("choose (complete, complete)"), "syntax");
    rejected(spec("complete and complete"), "syntax");
    rejected("language L { start E; syntax E = N(); }", "syntax");
    rejected("language L { start Missing; syntax E = N(); rule N = complete; }", "start sort");
    rejected("language L { start E; syntax E = N(); rule X = complete; }", "unknown constructor");
    rejected("language L { start E; syntax E = N() | M(); rule N = complete; }", "missing rule");
    rejected("language L { start E; syntax E = N(); rule N = complete; rule N = fail; }", "duplicate rule");
    rejected("language L { start E; syntax E = N(); syntax E = M(); rule N = complete; }", "duplicate sort");
    rejected("language L { start E; syntax E = N() | N(); rule N = complete; }", "duplicate constructor");
    rejected("language L { start E; syntax E = N(x: integer, x: integer); rule N = complete; }", "duplicate field");
    rejected("language L { start E; syntax E = N(x: Unknown); rule N = complete; }", "unknown field sort");
    rejected("language L { start E; syntax E = N(x: E); rule N = evaluate(x); }", "uninhabited");
    rejected("language L { start E; syntax E = N() | M(x: E); rule N = complete; rule M = give x; }", "evaluate(field)");
    rejected(std::string(65537, ' '), "64 KiB");
    rejected(spec("complete") + std::string(1, '\0'), "NUL");
    auto grouped = [](std::size_t depth) {
      return spec(std::string(depth, '(') + "complete" + std::string(depth, ')'));
    };
    check(parse(grouped(253)).name() == "Test", "valid grouping near parser depth limit");
    rejected(grouped(254), "syntax");
    rejected(grouped(600), "syntax");
    check(parse("# " + std::string(600, '(') + "\n" + spec("complete")).name() == "Test",
          "parentheses in layout must not count toward syntax nesting");
    auto bad = "language L {\n start E;\n syntax E = N();\n rule N = give missing;\n}";
    try { (void)parse(bad); check(false, "accepted missing field"); }
    catch (const ParseError& e) { check(e.line == 4 && e.column == 16, "semantic source span"); }
    std::vector<std::future<void>> jobs;
    for (int i = 0; i < 8; ++i) jobs.push_back(std::async(std::launch::async, [] {
      for (int j = 0; j < 20; ++j) {
        auto language = parse(spec("give n + 1 * 2"));
        check(!generate(language, Target::C).empty(), "concurrent parse");
        (void)novostone::datalog::parse(".decl R(integer). R(1).");
      }
    }));
    for (auto& job : jobs) job.get();
    try { (void)target("cpp"); check(false, "unknown target"); } catch (const std::invalid_argument&) {}
    std::cout << "ActionSemantics frontend checks passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
