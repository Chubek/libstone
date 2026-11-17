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
}
int main() {
  try {
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
    rejected(spec("unfold"), "syntax");
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
    rejected(spec(std::string(600, '(') + "complete" + std::string(600, ')')), "syntax");
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
