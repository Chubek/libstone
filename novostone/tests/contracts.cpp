#include "novostone/CostCC/CostCC.hpp"
#include "novostone/SpeculativeIR/SpeculativeIR.hpp"

#include <atomic>
#include <barrier>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <thread>

using namespace novostone;
namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F action, const std::string& message) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  require(rejected, message);
}
datalog::Program graph_program() {
  using namespace datalog;
  Program p;
  auto I = Type::Integer;
  p.declare("Edge", {I, I}); p.declare("Node", {I}); p.declare("Note", {Type::Symbol});
  p.declare("Reach", {I, I}, true); p.declare("Sink", {I}, true);
  auto v = Term::var;
  p.add({{"Reach", {v("a"), v("b")}}, {{"Edge", {v("a"), v("b")}}}});
  p.add({{"Reach", {v("a"), v("c")}}, {{"Reach", {v("a"), v("b")}}, {"Edge", {v("b"), v("c")}}}});
  p.add({{"Sink", {v("a")}}, {{"Node", {v("a")}}, {"Edge", {v("a"), Term::any()}, true}}});
  return p;
}
void datalog_tests() {
  using namespace datalog;
  auto p = graph_program();
  Facts facts{{"Edge", {{1LL, 2LL}, {2LL, 3LL}, {3LL, 2LL}}}, {"Node", {{1LL}, {2LL}, {3LL}, {4LL}}}};
  auto reach = p.evaluate(facts, "Reach");
  require(reach.rows == Relation{{1LL, 2LL}, {1LL, 3LL}, {2LL, 2LL}, {2LL, 3LL}, {3LL, 2LL}, {3LL, 3LL}}, "recursive least fixed point");
  require(reach.dependencies == std::set<std::string>{"Edge"}, "transitive base dependencies");
  require(p.evaluate(facts, "Sink").rows == Relation{{4LL}}, "stratified wildcard negation");
  rejects([&] { p.evaluate(facts, "Reach", {2, 100}); }, "fact budget");
  rejects([&] { p.evaluate(facts, "Reach", {100, 1}); }, "work budget");
  rejects([&] { p.validate_fact("Edge", {std::string("wrong"), 1LL}); }, "fact type check");
  rejects([&] { p.validate_fact("Reach", {1LL, 2LL}); }, "derived writes rejected");
  auto unsafe = p;
  unsafe.add({{"Sink", {Term::var("x")}}, {{"Node", {Term::var("x")}, true}}});
  rejects([&] { unsafe.validate(); }, "unsafe negation rejected");
  auto cycle = p;
  cycle.add({{"Sink", {Term::var("x")}}, {{"Node", {Term::var("x")}}, {"Sink", {Term::var("x")}, true}}});
  rejects([&] { cycle.validate(); }, "negative recursion rejected");
  auto typed = p;
  typed.add({{"Sink", {Term::var("x")}}, {{"Note", {Term::var("x")}}}});
  rejects([&] { typed.validate(); }, "cross-relation variable types");
  Program repeated;
  repeated.declare("Pair", {Type::Symbol, Type::Symbol}); repeated.declare("Same", {Type::Symbol}, true);
  repeated.add({{"Same", {Term::var("x")}}, {{"Pair", {Term::var("x"), Term::var("x")}}}});
  require(repeated.evaluate({{"Pair", {{std::string("a"), std::string("a")}, {std::string("a"), std::string("b")}}}}, "Same").rows == Relation{{std::string("a")}}, "repeated variable equality");
  std::mt19937 rng(721);
  for (int trial = 0; trial < 40; ++trial) {
    Facts f; bool reference[8][8]{};
    for (int a = 0; a < 8; ++a) for (int b = 0; b < 8; ++b) if (rng() % 5 == 0) {
      f["Edge"].insert({std::int64_t{a}, std::int64_t{b}}); reference[a][b] = true;
    }
    for (int k = 0; k < 8; ++k) for (int a = 0; a < 8; ++a) for (int b = 0; b < 8; ++b)
      reference[a][b] = reference[a][b] || (reference[a][k] && reference[k][b]);
    Relation expected;
    for (int a = 0; a < 8; ++a) for (int b = 0; b < 8; ++b) if (reference[a][b]) expected.insert({std::int64_t{a}, std::int64_t{b}});
    require(p.evaluate(f, "Reach").rows == expected, "random graph differential closure");
  }
}
void ccdb_tests() {
  ccdb::Database db(graph_program());
  auto tx = db.begin(); tx.insert("Edge", {1LL, 2LL}); tx.insert("Edge", {2LL, 3LL});
  for (std::int64_t id = 1; id <= 3; ++id) tx.insert("Node", {id});
  auto old = tx.commit("initial graph");
  require(old.query("Reach").rows.count({1LL, 3LL}), "initial analysis");
  require(old.query("Reach").cached, "query caching");
  auto unrelated = db.begin(); unrelated.insert("Note", {std::string("profile")});
  auto noted = unrelated.commit("unrelated edit");
  require(noted.query("Reach").cached, "unaffected analysis retained");
  auto before_sink = noted.query("Sink");
  require(before_sink.rows == datalog::Relation{{3LL}}, "initial sink");
  auto remove = db.begin(); remove.erase("Edge", {2LL, 3LL});
  auto edited = remove.commit("delete edge");
  require(!edited.query("Reach").rows.count({1LL, 3LL}), "deletion retracts derived facts");
  require(edited.query("Sink").rows == datalog::Relation{{2LL}, {3LL}}, "negation invalidated by deletion");
  require(old.query("Reach").rows.count({1LL, 3LL}), "old snapshot remains coherent");
  rejects([&] { edited.query("Reach", {100, 0}); }, "cache cannot bypass budgets");
  auto bad = db.begin(); bad.insert("Edge", {3LL, 4LL});
  rejects([&] { bad.commit("invalid", [](const auto&) { throw std::invalid_argument("validator rejected"); }); }, "validator rejection");
  require(db.snapshot().same_revision(edited), "rejected transaction leaves public state intact");
  bad.abort(); rejects([&] { bad.commit(); }, "closed transaction");
  rejects([&] { db.begin(old); }, "stale work rejected");
  ccdb::Database other(graph_program());
  rejects([&] { other.begin(edited); }, "foreign snapshot rejected");
  std::barrier start(2); std::atomic<int> committed = 0, conflicts = 0;
  auto writer = [&](std::int64_t id) {
    auto work = db.begin(); work.insert("Node", {id}); start.arrive_and_wait();
    try { work.commit("parallel edit"); ++committed; } catch (const ccdb::Conflict&) { ++conflicts; }
  };
  std::thread first(writer, 10), second(writer, 20); first.join(); second.join();
  require(committed == 1 && conflicts == 1, "serializable concurrent publication");
  require(db.events().size() == 4, "audit log contains committed revisions");
  std::atomic<bool> coherent = true;
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) readers.emplace_back([&] {
    for (int j = 0; j < 30; ++j) if (!old.query("Reach").rows.count({1LL, 3LL})) coherent = false;
  });
  for (auto& reader : readers) reader.join();
  require(coherent, "parallel immutable reads");
  auto reservation = db.begin(); reservation.reserve(500); reservation.abort();
  require(db.allocate() == 501, "aborted imported identities are not recycled");
}
ccdb::Snapshot arithmetic(ccdb::Database& db) {
  ir::Builder b(db, 1); auto block = b.block();
  auto p = b.emit(block, ir::Op::Param); auto zero = b.emit(block, ir::Op::Const);
  auto sum = b.emit(block, ir::Op::Add, {p, zero}); b.emit(block, ir::Op::Return, {sum});
  return b.finish();
}
void ir_tests() {
  ccdb::Database db(ir::program(), [](const auto& s) { ir::verify(s); });
  auto snapshot = arithmetic(db); auto f = ir::load(snapshot);
  require(ir::evaluate(f, {UINT32_MAX}).value == UINT32_MAX, "parameter/add reference execution");
  require(ir::evaluate(f).trap == "argument count", "explicit function signature");
  require(ir::dump(ir::parse(ir::dump(f))) == ir::dump(f), "text roundtrip");
  rejects([&] { ir::parse(ir::dump(f) + "garbage"); }, "invalid text rejected");
  auto bad = f; bad.blocks.begin()->second.instructions[2].operands[0] = 999;
  rejects([&] { ir::publish(db, snapshot, bad, "bad SSA"); }, "undefined SSA rejected");
  require(db.snapshot().same_revision(snapshot), "invalid IR not published");
  bad = f; bad.blocks.begin()->second.instructions[2].operands[0] = bad.blocks.begin()->second.instructions[2].id;
  rejects([&] { ir::verify(bad); }, "self-use rejected");
  bad = f; bad.blocks.begin()->second.instructions.pop_back();
  rejects([&] { ir::verify(bad); }, "missing terminator rejected");
  bad = f; bad.blocks.begin()->second.instructions[1].id = bad.entry;
  rejects([&] { ir::verify(bad); }, "global entity identity enforced");
  ir::Function diamond{1, 1, {
    {1, {1, {{5, ir::Op::Param, 0, {}, {}}, {6, ir::Op::Branch, 0, {5}, {2, 3}}}}},
    {2, {2, {{7, ir::Op::Const, 42, {}, {}}, {8, ir::Op::Jump, 0, {}, {4}}}}},
    {3, {3, {{9, ir::Op::Jump, 0, {}, {4}}}}},
    {4, {4, {{10, ir::Op::Return, 0, {7}, {}}}}}}};
  rejects([&] { ir::verify(diamond); }, "non-dominating branch definition rejected");
  diamond.blocks.at(4).instructions[0].operands = {5}; ir::verify(diamond);
  require(ir::evaluate(diamond, {0}).value == 0 && ir::evaluate(diamond, {1}).value == 1, "both branch paths execute");
  auto dom = ir::dominators(diamond);
  require(dom.at(4) == std::set<ir::Id>{1, 4}, "diamond dominance");
  ir::Function loop{0, 1, {{1, {1, {{2, ir::Op::Jump, 0, {}, {1}}}}}}};
  require(ir::evaluate(loop, {}, {}, 7).trap == "step budget", "loop execution bounded");
  ccdb::Database memory_db(ir::program()); ir::Builder b(memory_db);
  auto block = b.block(); auto address = b.emit(block, ir::Op::Const);
  auto maximum = b.emit(block, ir::Op::Const, {}, UINT32_MAX); auto one = b.emit(block, ir::Op::Const, {}, 1);
  auto wrap = b.emit(block, ir::Op::Add, {maximum, one});
  b.emit(block, ir::Op::Store, {address, wrap}); auto read = b.emit(block, ir::Op::Load, {address}); b.emit(block, ir::Op::Return, {read});
  auto memory = ir::load(b.finish()); auto result = ir::evaluate(memory, {}, {9});
  require(result.value == 0 && result.state.memory == std::vector<std::uint32_t>{0} && result.state.writes == std::vector<ir::Write>{{0, 0}}, "wrapping arithmetic and memory trace");
  require(ir::evaluate(memory).trap == "memory bounds", "checked memory trap");
  ccdb::Database imported(ir::program());
  ir::publish(imported, imported.snapshot(), memory, "import function");
  require(imported.allocate() > memory.blocks.begin()->second.instructions.back().id, "imported instruction identities reserved");
}
void query_tests() {
  ccdb::Database db(ir::program()); auto source = arithmetic(db);
  auto optimized = query::optimize(source);
  require(optimized.rewrites().size() == 3 && !optimized.budget_exhausted(), "identity, copy, dead rewrite pipeline");
  require(query::optimize(source, 1).budget_exhausted(), "rewrite budget enforced");
  require(query::optimize(source, 0).rewrites().empty(), "zero rewrite budget");
  auto applied = query::apply(db, optimized);
  require(ir::load(applied).blocks.begin()->second.instructions.size() == 2, "optimized function published");
  rejects([&] { query::apply(db, optimized); }, "stale optimized candidate rejected");
  require(query::optimize(applied).rewrites().empty(), "optimization reaches fixed point");
  std::mt19937 rng(123456);
  const std::vector<ir::Op> ops{ir::Op::Add, ir::Op::Sub, ir::Op::Mul, ir::Op::Eq, ir::Op::Ult, ir::Op::Select};
  std::set<std::string> fired;
  for (int trial = 0; trial < 60; ++trial) {
    ccdb::Database random_db(ir::program()); ir::Builder b(random_db, 2); auto block = b.block();
    std::vector<ir::Id> values{b.emit(block, ir::Op::Param), b.emit(block, ir::Op::Param, {}, 1)};
    for (auto constant : {0u, 1u, UINT32_MAX, static_cast<std::uint32_t>(rng())}) values.push_back(b.emit(block, ir::Op::Const, {}, constant));
    for (int step = 0; step < 14; ++step) {
      auto op = ops[rng() % ops.size()];
      std::vector<ir::Id> operands{values[rng() % values.size()], values[rng() % values.size()]};
      if (op == ir::Op::Select) operands.push_back(values[rng() % values.size()]);
      values.push_back(b.emit(block, op, operands));
    }
    b.emit(block, ir::Op::Return, {values.back()});
    auto input = b.finish(); auto candidate = query::optimize(input);
    auto before = ir::load(input), after = ir::load(candidate.result());
    for (const auto& rewrite : candidate.rewrites()) fired.insert(rewrite.rule);
    for (int sample = 0; sample < 15; ++sample) {
      std::vector<std::uint32_t> args{static_cast<std::uint32_t>(rng()), static_cast<std::uint32_t>(rng())};
      require(ir::equivalent(ir::evaluate(before, args), ir::evaluate(after, args)), "seeded random differential optimizer: trial " + std::to_string(trial));
    }
  }
  require(fired.count("fold_add") && fired.count("fold_mul") && fired.count("dead_pure") && fired.count("select_value"), "random corpus exercises distinct rules");
  ccdb::Database trap_db(ir::program()); ir::Builder b(trap_db); auto block = b.block();
  auto zero = b.emit(block, ir::Op::Const); b.emit(block, ir::Op::Load, {zero}); b.emit(block, ir::Op::Return, {zero});
  auto trapping = b.finish(); auto preserved = query::optimize(trapping);
  require(ir::evaluate(ir::load(preserved.result())).trap == "memory bounds", "dead loads retain trapping effects");
  require(ir::equivalent(ir::evaluate(ir::load(trapping), {}, {17}), ir::evaluate(ir::load(preserved.result()), {}, {17})), "valid dead load behavior preserved");
}
void speculation_tests() {
  ccdb::Database db(ir::program()); ir::Builder b(db, 2); auto block = b.block();
  auto address = b.emit(block, ir::Op::Const); auto value = b.emit(block, ir::Op::Const, {}, 19);
  b.emit(block, ir::Op::Store, {address, value});
  auto x = b.emit(block, ir::Op::Param); auto y = b.emit(block, ir::Op::Param, {}, 1);
  auto sum = b.emit(block, ir::Op::Add, {x, y}); b.emit(block, ir::Op::Store, {address, sum}); b.emit(block, ir::Op::Return, {sum});
  auto source = b.finish(); auto reference = ir::load(source);
  auto assumptions = std::make_shared<speculative::Assumptions>(); auto a = assumptions->assume("stable arguments");
  auto version = speculative::Version::specialize(source, assumptions, {{x, 7, a}, {y, 9, a}});
  require(!version.execute({7, 9}, {0}).failed_guard, "specialized fast path");
  for (const auto& guard : version.guards()) {
    auto forced = version.execute({7, 9}, {0}, 1000, guard.id);
    require(forced.failed_guard == guard.id && forced.recovery.has_value(), "force each guard to fail");
    require(ir::equivalent(forced.outcome, ir::evaluate(reference, {7, 9}, {0})), "deopt does not repeat prior stores");
    require(forced.outcome.state.writes.size() == 2, "exactly-once visible effects");
  }
  for (std::uint32_t x_value = 0; x_value < 12; ++x_value) for (std::uint32_t y_value = 0; y_value < 12; ++y_value)
    require(ir::equivalent(version.execute({x_value, y_value}, {0}).outcome, ir::evaluate(reference, {x_value, y_value}, {0})), "guarded differential execution");
  require(version.execute({7, 9}).outcome.trap == "memory bounds", "pre-guard exception ordering");
  require(version.execute({}).outcome.trap == "argument count", "speculative signature check");
  rejects([&] { query::optimize(version.snapshot()); }, "generic optimizer cannot discard recovery obligations");
  ccdb::Database corrupt(ir::program()); auto tx = corrupt.begin();
  for (const auto& [name, rows] : version.snapshot().facts()) for (const auto& row : rows) tx.insert(name, row);
  auto binding = *version.snapshot().relation("Binding").begin(); tx.erase("Binding", binding);
  rejects([&] { speculative::verify(source, tx.preview()); }, "missing frame local rejected");
  tx.insert("Binding", binding);
  tx.erase("Guard", *version.snapshot().relation("Guard").begin());
  rejects([&] { speculative::verify(source, tx.preview()); }, "unguarded specialization rejected");
  assumptions->invalidate(a);
  auto invalidated = version.execute({7, 9}, {0});
  require(invalidated.reason == "assumption invalidated" && ir::equivalent(invalidated.outcome, ir::evaluate(reference, {7, 9}, {0})), "invalidation recovers to baseline");
  rejects([&] { speculative::Version::specialize(source, assumptions, {{x, 7, a}}); }, "cannot publish invalid assumption");
  auto live = assumptions->assume("concurrent invalidation");
  auto concurrent = speculative::Version::specialize(source, assumptions, {{x, 7, live}});
  std::atomic<bool> correct = true;
  std::thread executor([&] {
    for (int i = 0; i < 50; ++i) if (!ir::equivalent(concurrent.execute({7, 9}, {0}).outcome, ir::evaluate(reference, {7, 9}, {0}))) correct = false;
  });
  assumptions->invalidate(live); executor.join();
  require(correct && concurrent.execute({7, 9}, {0}).failed_guard.has_value(), "invalidation synchronizes execution");
  auto stats = version.statistics();
  require(stats.forced_failures == 2 && stats.invalidations == 1 && stats.predicate_failures > 0 &&
          stats.deoptimizations == stats.forced_failures + stats.invalidations + stats.predicate_failures, "guard behavior counters");
  rejects([&] { version.execute({7, 9}, {0}, 1000, 999999); }, "invalid forced guard ID rejected");
}
void cost_tests() {
  cost::Context context{"cpu-test", "scalar", "L1=32K", "latency", 4};
  std::vector<cost::Sample> training, calibration;
  std::vector<cost::Features> vectors{{0, 0, 0, 0}, {10, 0, 0, 0}, {0, 10, 0, 0}, {0, 0, 10, 0}, {0, 0, 0, 10}, {10, 10, 10, 10}};
  auto latency = [](const cost::Features& f) { return 10 + 2 * f[0] + 5 * f[1] + 8 * f[2] + 3 * f[3]; };
  for (std::size_t i = 0; i < vectors.size(); ++i) training.push_back({context, "train-" + std::to_string(i), vectors[i], latency(vectors[i])});
  for (auto f : {cost::Features{2, 3, 4, 5}, cost::Features{5, 4, 3, 2}}) calibration.push_back({context, "held-out", f, latency(f)});
  auto model = cost::Model::train("model-v1", training, calibration);
  auto prediction = model.predict(context, {3, 1, 2, 1});
  require(prediction && std::abs(prediction->latency - 40) < 0.01 && prediction->confidence > 0.99, "calibrated regression prediction");
  auto restored_model = cost::Model::parse(model.serialize());
  require(restored_model.serialize() == model.serialize() &&
          restored_model.predict(context, {3, 1, 2, 1})->latency == prediction->latency, "lossless versioned model serialization");
  rejects([&] { cost::Model::parse(model.serialize() + "trailing"); }, "trailing model data rejected");
  rejects([&] { cost::Model::parse(model.serialize().substr(0, 30)); }, "truncated model rejected");
  auto other_context = context; other_context.target = "unseen-cpu";
  require(!model.predict(other_context, {3, 1, 2, 1}), "target mismatch fallback");
  other_context = context; ++other_context.profile_epoch;
  require(!model.predict(other_context, {3, 1, 2, 1}), "stale profile fallback");
  require(!model.predict(context, {30, 1, 2, 1}), "out-of-distribution fallback");
  ccdb::Database db(ir::program()); auto source = arithmetic(db);
  std::vector<query::Candidate> candidates{query::baseline(source), query::optimize(source)};
  cost::Ranker ranker(model); auto chosen = ranker.choose(candidates, context);
  require(chosen.used_model && chosen.selected == 1, "model ranks legal candidates");
  ranker.observe(0, 1000);
  require(ranker.log()[0].drift && ranker.log()[0].observed_latency == 1000, "measurement drift logged");
  require(ranker.quarantined() && !ranker.choose(candidates, context).used_model, "drift quarantines model on known target");
  rejects([&] { ranker.replace_model(model); }, "cannot silently replace model under the same version");
  ranker.replace_model(cost::Model::train("model-v2", training, calibration));
  require(!ranker.quarantined() && ranker.choose(candidates, context).used_model, "fresh calibrated model leaves quarantine");
  ranker.observe(0, 2000);
  require(!ranker.quarantined(), "old model feedback cannot quarantine replacement");
  auto fallback = ranker.choose(candidates, other_context);
  require(!fallback.used_model && fallback.selected == 1, "deterministic heuristic fallback");
  auto poor_calibration = calibration;
  for (auto& sample : poor_calibration) sample.latency *= 100;
  cost::Ranker uncertain(cost::Model::train("uncertain", training, poor_calibration));
  require(!uncertain.choose(candidates, context).used_model, "low confidence fallback");
  auto leak = calibration; leak[0].workload = training[0].workload;
  rejects([&] { cost::Model::train("leak", training, leak); }, "training/calibration leakage rejected");
  auto invalid = training; invalid[0].latency = std::numeric_limits<double>::quiet_NaN();
  rejects([&] { cost::Model::train("nan", invalid, calibration); }, "non-finite measurement rejected");
  ccdb::Database other(ir::program()); auto foreign = arithmetic(other);
  rejects([&] { ranker.choose({candidates[0], query::baseline(foreign)}, context); }, "cannot rank different semantic roots");
  rejects([&] { ranker.choose({}, context); }, "empty candidate set rejected");
  require(!chosen.explain().empty(), "decision inspection");
}
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("expected test suite name");
    std::string suite = argv[1];
    if (suite == "datalog") datalog_tests();
    else if (suite == "ccdb") ccdb_tests();
    else if (suite == "ir") ir_tests();
    else if (suite == "query") query_tests();
    else if (suite == "speculation") speculation_tests();
    else if (suite == "cost") cost_tests();
    else throw std::invalid_argument("unknown test suite");
    std::cout << suite << " contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "contract failure: " << error.what() << '\n';
    return 1;
  }
}
