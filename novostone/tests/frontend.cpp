#include "novostone/CostCC/CostCC.hpp"
#include "novostone/SpeculativeIR/SpeculativeIR.hpp"

#include <atomic>
#include <iostream>
#include <limits>
#include <locale>
#include <random>
#include <thread>

using namespace novostone;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* message) {
  try { action(); } catch (const std::invalid_argument&) { return; }
  throw std::runtime_error(message);
}
const char* graph = R"(
# Declarations may appear after their uses.
Reach(X, Z) :- Reach(X, Y), Reach(Y, Z).
Reach(X, Y) :- Mirror(X, Y).
Mirror(X, Y) :- Reach(X, Y).
Reach(X, Y) :- Edge(X, Y).
Sink(X) :- !Edge(X, _), Node(X).
.decl Edge(src: integer, dst: integer).
.decl Node(integer).
.decl Reach(integer, integer) derived.
.decl Mirror(integer, integer) derived.
.decl Sink(integer) derived.
Edge(1, 2). Edge(2, 3). Edge(3, 2). Edge(1, 2).
Node(1). Node(2). Node(3). Node(4).
)";
void frontend() {
  using namespace datalog;
  auto module = parse(graph, "graph.dl");
  auto reach = module.program.evaluate(module.facts, "Reach");
  check(reach.rows == Relation{{1LL, 2LL}, {1LL, 3LL}, {2LL, 2LL}, {2LL, 3LL}, {3LL, 2LL}, {3LL, 3LL}},
        "parsed nonlinear mutual recursion");
  check(module.program.evaluate(module.facts, "Reach", {}, Evaluation::Naive).rows == reach.rows, "parsed naive reference");
  check(module.program.evaluate(module.facts, "Sink").rows == Relation{{4LL}}, "parsed safe reordered negation");
  auto text = dump(module), normalized = dump(parse(text));
  check(text == normalized, "deterministic source normalization");
  check(module.program.signature() == parse(text).program.signature(), "source preserves exact rule identity");
  check(module.facts == parse(text).facts, "source preserves fact sets");

  auto symbols = parse(R"(.decl S(symbol, integer).
S("quote\" slash\\ newline\n tab\t nul\0 byte\xff", -9223372036854775808).
S("", 9223372036854775807).
.decl Equal(symbol) derived.
Equal(X) :- S(X, _).
.decl Flag() derived.
Flag() :- .
.decl Unit(). Unit().
.decl Present() derived. Present() :- Unit().
)");
  auto roundtrip = parse(dump(symbols));
  check(roundtrip.facts == symbols.facts, "binary symbols and integer boundaries round-trip");
  check(roundtrip.program.evaluate(roundtrip.facts, "Flag").rows == Relation{Tuple{}}, "empty-body seed");
  check(roundtrip.program.evaluate(roundtrip.facts, "Present").rows == Relation{Tuple{}}, "nullary relations");
  check(parse("").program.schemas().empty(), "empty module");
  check(parse("# only a comment").program.schemas().empty(), "comment at EOF");
  check(parse("// only a comment\r\n").program.schemas().empty(), "CRLF comment");
  auto underscore = parse(".decl E(integer). E(3). .decl R(integer) derived. R(_x) :- E(_x).");
  check(underscore.program.evaluate(underscore.facts, "R").rows == Relation{{3LL}}, "underscore variable is not wildcard prefix");

  // The C++ API permits arbitrary names. Quoted relation and variable syntax
  // must not change their identity or turn a variable into a constant.
  Module unusual;
  std::string name = "odd\nname", variable = "_";
  unusual.program.declare(name, {Type::Symbol});
  unusual.program.declare("answer", {Type::Symbol}, true);
  unusual.program.add({{"answer", {Term::var(variable)}}, {{name, {Term::var(variable)}}}});
  unusual.facts[name].insert({std::string("a\0b", 3)});
  auto imported = parse(dump(unusual));
  check(imported.program.signature() == unusual.program.signature() && imported.facts == unusual.facts, "arbitrary API names");
  check(imported.program.evaluate(imported.facts, "answer").rows == unusual.facts.at(name), "quoted variable evaluation");

  for (const auto* invalid : {
    ".decl X(integer)", ".declX(integer).", ".decl X(unknown).", ".decl X(a: integer, a: integer).",
    ".decl X(integer). .decl X(integer).", "X(1).", ".decl X(integer). X().",
    ".decl X(integer). X(\"wrong\").", ".decl X(integer). X(9223372036854775808).",
    ".decl X(integer). X(-9223372036854775809).", ".decl X(integer). X(1junk).",
    ".decl X(integer). X(Y).", ".decl X(integer). X(_).", ".decl X(integer) derived. X(1).",
    ".decl X(integer). X(A) :- X(A).", ".decl X(integer) derived. X(A) :- .",
    ".decl X(integer) derived. X(_) :- .", ".decl X(integer) derived. X(A) :- !X(A).",
    ".decl E(integer). .decl X(integer) derived. X(A) :- E(A), !X(A).",
    ".decl E(integer). .decl X(symbol) derived. X(A) :- E(A).",
    ".decl X(integer) derived. X(A) :- Missing(A).", ".decl X(symbol). X(\"bad\\q\").",
    ".decl X(symbol). X(\"bad\\x1\").", ".decl X(symbol). X(\"unterminated).",
    ".decl X(integer). X(1). trailing", ".include \"arbitrary-file\".", ".decl X(integer). !X(1).",
    ".decl X(integer). X(1) :- X(1),."
  }) rejects([&] { (void)parse(invalid, "bad.dl"); }, "malformed source accepted");
  rejects([&] { (void)parse(std::string("\0junk", 5)); }, "NUL truncation");
  rejects([&] { (void)parse(graph, "budget.dl", {4, 100}); }, "source byte cap");
  rejects([&] { (void)parse(graph, "budget.dl", {10000, 1}); }, "statement cap");
  std::string huge = ".decl X() derived. X() :- X()";
  for (int i = 0; i < 256; ++i) huge += ", X()";
  rejects([&] { (void)parse(huge + '.'); }, "body depth cap");
  try {
    (void)parse(".decl X(integer).\nX(\"wrong\").", "location.dl");
    throw std::runtime_error("missing source error");
  } catch (const ParseError& e) {
    check(e.source == "location.dl" && e.line == 2 && e.column == 1 && e.offset == 18, "semantic source location");
    check(std::string(e.what()).find("location.dl:2:1:") == 0, "actionable diagnostic");
  }
  try {
    (void)parse(".decl X(integer).\nX(1", "syntax.dl");
    throw std::runtime_error("missing syntax error");
  } catch (const ParseError& e) {
    check(e.source == "syntax.dl" && e.line == 2 && e.column == 4 && e.offset == 21, "furthest syntax error location");
  }

  std::mt19937 random(728);
  for (int trial = 0; trial < 500; ++trial) {
    std::string damaged = graph;
    auto at = random() % damaged.size();
    if (trial % 3 == 0) damaged.erase(at, 1 + random() % 8);
    else if (trial % 3 == 1) damaged.insert(at, 1, static_cast<char>(random() % 128));
    else damaged[at] = static_cast<char>(random() % 128);
    try {
      auto parsed = parse(damaged);
      auto restored = parse(dump(parsed));
      check(parsed.program.signature() == restored.program.signature() && parsed.facts == restored.facts,
            "mutated accepted source preserves semantics");
    } catch (const ParseError&) { /* Malformed input must fail without leaking arena alternatives. */ }
  }

  // Long flat input must not consume native recursion proportional to facts.
  std::string flat = ".decl E(integer).\n";
  for (int i = 0; i < 2000; ++i) flat += "E(" + std::to_string(i) + ").\n";
  check(parse(flat).facts.at("E").size() == 2000, "iterative Aurocks list parsing");
  std::atomic<bool> okay{true}; std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
    try {
      for (int j = 0; j < 20; ++j) {
        auto parsed = parse(graph);
        if (parsed.program.evaluate(parsed.facts, "Reach").rows != reach.rows) okay = false;
        rejects([&] { (void)parse("broken("); }, "interleaved failed parse");
      }
    } catch (...) { okay = false; }
  });
  for (auto& thread : threads) thread.join();
  check(okay, "concurrent successful and failed parses");
}

void integration() {
  auto module = datalog::parse(graph);
  ccdb::Database database(module.program);
  auto transaction = database.begin(); transaction.insert(module.facts);
  auto initial = transaction.commit("parsed source");
  check(initial.query("Reach").rows.size() == 6, "parsed CCDB import");
  (void)initial.query("Reach");
  auto edit = database.begin();
  rejects([&] { edit.insert(datalog::Facts{{"Edge", {{9LL, 10LL}}}, {"Node", {{std::string("bad")}}}}); }, "invalid batch");
  check(!edit.relation("Edge").count({9LL, 10LL}), "failed import leaves no partial facts");
  rejects([&] { edit.insert(datalog::Facts{{"Unknown", {}}}); }, "empty unknown relation");
  rejects([&] { edit.insert(datalog::Facts{{"Reach", {}}}); }, "empty derived relation");
  edit.erase("Edge", {2LL, 3LL}); auto deleted = edit.commit();
  check(deleted.query("Reach").rows == datalog::Relation{{1LL, 2LL}, {3LL, 2LL}}, "parsed recursive deletion invalidation");
  check(initial.query("Reach").rows.size() == 6, "parsed snapshot persistence");
  ccdb::Database restored(datalog::parse(datalog::dump(module)).program);
  auto recovered = restored.restore(restored.snapshot(), database.checkpoint());
  check(recovered.query("Reach").rows == deleted.query("Reach").rows, "source normalization preserves checkpoint identity");

  ccdb::Database db(ir::program()); ir::Builder builder(db, 1);
  auto block = builder.block(), parameter = builder.emit(block, ir::Op::Param, {}, 0);
  auto zero = builder.emit(block, ir::Op::Const, {}, 0);
  auto sum = builder.emit(block, ir::Op::Add, {parameter, zero});
  builder.emit(block, ir::Op::Return, {sum});
  auto source = builder.finish();
  check(source.query("CopyCandidate").rows.count({sum, std::string("add_zero"), parameter}), "declarative IR rewrite opportunity");
  auto optimized = query::optimize(source);
  check(query::validate(ir::load(source), ir::load(optimized.result()), query::corpus(1, 728)).verdict == query::Verdict::Passed,
        "parsed rules drive verified optimization");

  auto changed = ir::load(source); changed.blocks.at(block).instructions.at(1).immediate = 1;
  rejects([&] { (void)query::validate(ir::load(source), changed, {{{5}, {}}, {{}, {}}}); },
          "a mismatch must not hide malformed later corpus entries");
  for (auto invalid : {static_cast<ir::Op>(-1), static_cast<ir::Op>(999)}) {
    rejects([&] { (void)ir::pure(invalid); }, "invalid opcode classified pure");
    rejects([&] { (void)ir::produces_value(invalid); }, "invalid opcode produces value");
    rejects([&] { (void)ir::effects(invalid); }, "invalid opcode classified effect-free");
  }
  auto assumptions = std::make_shared<speculative::Assumptions>();
  auto assumption = assumptions->assume("input is often 5");
  auto version = speculative::Version::specialize(source, assumptions, {{parameter, 5, assumption}});
  check(version.execute({5}).outcome.value == 5 && version.execute({6}).outcome.value == 6, "parsed IR guard recovery");
  rejects([&] { speculative::verify(version.snapshot(), version.snapshot()); }, "guarded baseline must not certify a refinement");
  ccdb::Database guarded(ir::program()); auto seed = guarded.begin(); seed.insert(version.snapshot().facts());
  auto fast = seed.commit();
  auto overwrite = guarded.begin();
  rejects([&] { ir::store(overwrite, ir::load(source)); }, "core replacement must not keep stale recovery metadata");
  check(overwrite.preview().facts() == fast.facts(), "rejected guarded replacement leaves transaction unchanged");
  overwrite.abort();
  check(guarded.snapshot().same_revision(fast), "guarded publication unchanged");

  auto make_model = [](const char* name) {
    const cost::Context context{"test", "i32", "none", "unit", 0};
    std::vector<cost::Sample> training;
    for (int i = 0; i < 5; ++i) training.push_back({context, "train" + std::to_string(i), {double(i), 0, 0, 0}, double(i + 1)});
    return cost::Model::train(name, training, {{context, "cal1", {1, 0, 0, 0}, 2}, {context, "cal2", {2, 0, 0, 0}, 3}});
  };
  auto first = make_model("one"), second = make_model("two");
  cost::Ranker ranker(first); ranker.replace_model(second);
  rejects([&] { ranker.replace_model(first); }, "retired model identity reused without logged decisions");
  rejects([&] { ranker.replace_model(second); }, "current model identity reused");
  ranker.replace_model(make_model("three"));
  rejects([&] { (void)cost::heuristic({0, std::numeric_limits<double>::max(), 0, 0}); }, "heuristic overflow");
  cost::Ranker fallback;
  auto decision = fallback.choose({query::baseline(source), optimized}, {"test", "i32", "none", "unit", 0});
  check(!decision.used_model && decision.selected == 1, "end-to-end deterministic cost choice");
}
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("expected suite name");
    if (std::string(argv[1]) == "frontend") frontend();
    else if (std::string(argv[1]) == "integration") integration();
    else throw std::invalid_argument("unknown suite");
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
