#include "novostone/CostCC/CostCC.hpp"
#include "novostone/SpeculativeIR/SpeculativeIR.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <iostream>
#include <random>
#include <thread>

using namespace novostone;
namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action, const std::string& message) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  check(rejected, message);
}
bool fired(const query::Candidate& candidate, const std::string& name) {
  return std::any_of(candidate.rewrites().begin(), candidate.rewrites().end(), [&](const auto& r) { return r.rule == name; });
}
datalog::Program graph(bool nonlinear = false) {
  using namespace datalog;
  Program p; auto I = Type::Integer;
  p.declare("Edge", {I, I}); p.declare("Node", {I}); p.declare("Left", {I}); p.declare("Right", {I}); p.declare("Note", {I});
  p.declare("Reach", {I, I}, true); p.declare("Mirror", {I, I}, true); p.declare("Sink", {I}, true);
  p.declare("Blocked", {I}, true); p.declare("Allowed", {I}, true); p.declare("SafePair", {I, I}, true);
  auto v = Term::var;
  // Deliberately put recursive rules before seeds; evaluation cannot depend on order.
  p.add({{"Reach", {v("a"), v("c")}}, {{"Reach", {v("a"), v("b")}},
    {nonlinear ? "Reach" : "Edge", {v("b"), v("c")}}}});
  p.add({{"Mirror", {v("a"), v("b")}}, {{"Reach", {v("a"), v("b")}}}});
  p.add({{"Reach", {v("a"), v("b")}}, {{"Mirror", {v("a"), v("b")}}}});
  p.add({{"Reach", {v("a"), v("b")}}, {{"Edge", {v("a"), v("b")}}}});
  p.add({{"Sink", {v("a")}}, {{"Node", {v("a")}}, {"Edge", {v("a"), Term::any()}, true}}});
  p.add({{"Blocked", {v("a")}}, {{"Reach", {v("a"), v("a")}}}});
  p.add({{"Allowed", {v("a")}}, {{"Node", {v("a")}}, {"Blocked", {v("a")}, true}}});
  p.add({{"SafePair", {v("a"), v("b")}}, {{"Reach", {v("a"), v("b")}}, {"Allowed", {v("a")}}, {"Blocked", {v("b")}, true}}});
  return p;
}
void seminaive() {
  using namespace datalog;
  std::mt19937 random(358);
  for (bool nonlinear : {false, true}) for (int trial = 0; trial < 35; ++trial) {
    auto p = graph(nonlinear); Facts facts;
    for (std::int64_t a = 0; a < 7; ++a) {
      facts["Node"].insert({a});
      for (std::int64_t b = 0; b < 7; ++b) if (random() % 5 == 0) facts["Edge"].insert({a, b});
    }
    for (const auto& goal : {"Reach", "Mirror", "Sink", "Allowed", "SafePair"}) {
      auto semi = p.evaluate(facts, goal), naive = p.evaluate(facts, goal, {}, Evaluation::Naive);
      check(semi.rows == naive.rows && semi.dependencies == naive.dependencies, "semi-naive/naive differential recursion and negation");
      check(semi.statistics.rule_evaluations && semi.statistics.rounds, "observable recursive evaluation statistics");
    }
  }
  auto p = graph(); Facts chain;
  for (std::int64_t a = 0; a < 40; ++a) chain["Edge"].insert({a, a + 1});
  auto semi = p.evaluate(chain, "Reach"), naive = p.evaluate(chain, "Reach", {}, Evaluation::Naive);
  check(semi.rows.size() == 820 && semi.rows == naive.rows, "long-chain closure result");
  check(semi.statistics.tuple_visits < naive.statistics.tuple_visits, "delta evaluation reduces repeated tuple visits on a chain");
  Program seed;
  seed.declare("Unit", {}); seed.declare("Flag", {}, true); seed.declare("Constant", {Type::Integer}, true);
  seed.add({{"Flag", {}}, {{"Unit", {}}}});
  seed.add({{"Constant", {Term::constant(std::int64_t{17})}}, {}});
  check(seed.evaluate({{"Unit", {Tuple{}}}}, "Flag").rows == Relation{Tuple{}}, "zero-arity delta relation");
  check(seed.evaluate({}, "Constant").rows == Relation{{17LL}}, "empty-body seed rule");
  rejects([&] { p.evaluate(chain, "Reach", {}, static_cast<Evaluation>(99)); }, "invalid evaluator mode rejected");
  ccdb::Database database(graph(true)); auto tx = database.begin();
  tx.insert("Edge", {1LL, 2LL}); tx.insert("Edge", {2LL, 1LL}); auto before = tx.commit();
  check(before.query("Reach").rows.size() == 4, "cyclic nonlinear closure");
  auto edit = database.begin(); edit.erase("Edge", {2LL, 1LL}); auto after = edit.commit();
  check(after.query("Reach").rows == Relation{{1LL, 2LL}}, "deletion retracts mutually recursive consequences");
}
void transactions() {
  ccdb::Database database(graph()); auto seed = database.begin();
  seed.insert("Left", {0LL}); seed.insert("Right", {0LL}); seed.insert("Node", {1LL}); auto source = seed.commit();
  std::barrier start(2); std::atomic<int> commits = 0, conflicts = 0;
  auto worker = [&](const std::string& name) {
    auto tx = database.begin(); (void)tx.relation(name);
    tx.erase(name, {0LL}); tx.insert(name, {1LL}); start.arrive_and_wait();
    try { tx.commit_disjoint("independent worker"); ++commits; } catch (const ccdb::Conflict&) { ++conflicts; }
  };
  std::thread left(worker, "Left"), right(worker, "Right"); left.join(); right.join();
  check(commits == 2 && conflicts == 0, "independent read/write sets both publish serializably");
  auto merged = database.snapshot();
  check(merged.relation("Left").count({1LL}) && merged.relation("Right").count({1LL}), "rebase preserves both changes");
  auto events = database.events();
  check(events.back().source == source.revision() && events.back().parent == source.revision() + 1, "audit distinguishes analysis source and actual parent");

  auto first = database.begin(), second = database.begin();
  (void)first.relation("Right"); (void)second.relation("Left");
  first.insert("Left", {2LL}); second.insert("Right", {2LL}); first.commit_disjoint();
  rejects([&] { second.commit_disjoint(); }, "read/write skew rejected");
  auto lost_a = database.begin(), lost_b = database.begin(); lost_a.insert("Left", {3LL}); lost_b.insert("Left", {4LL});
  lost_a.commit_disjoint(); rejects([&] { lost_b.commit_disjoint(); }, "same-relation lost update rejected");
  auto aba = database.begin(); (void)aba.relation("Right"); aba.insert("Note", {1LL});
  auto erase = database.begin(); erase.erase("Right", {1LL}); erase.commit();
  auto restore = database.begin(); restore.insert("Right", {1LL}); restore.commit();
  rejects([&] { aba.commit_disjoint(); }, "relation version detects ABA edits");
  auto negative = database.begin(); check(negative.query("Sink").rows.count({1LL}), "tracked negative query input");
  negative.insert("Note", {2LL}); auto edge = database.begin(); edge.insert("Edge", {1LL, 2LL}); edge.commit();
  rejects([&] { negative.commit_disjoint(); }, "negative query dependency participates in conflict detection");
  auto before_preview = database.begin(); (void)before_preview.preview(); before_preview.insert("Left", {5LL});
  auto unrelated = database.begin(); unrelated.insert("Note", {3LL}); unrelated.commit();
  rejects([&] { before_preview.commit_disjoint(); }, "unrestricted preview conservatively reads every relation");
  auto base = database.snapshot(); auto external = database.begin(); external.depend(base, {"Reach"}); external.insert("Note", {4LL});
  auto more_edges = database.begin(); more_edges.insert("Edge", {2LL, 3LL}); more_edges.commit();
  rejects([&] { external.commit_disjoint(); }, "external analysis dependencies expand transitively");
  rejects([&] { external.depend(database.snapshot(), {"Left"}); }, "cannot mix read revisions");

  auto validation = database.begin(); validation.insert("Right", {7LL});
  auto concurrent = database.begin(); concurrent.insert("Left", {7LL}); concurrent.commit();
  bool saw_merged = false;
  auto validated = validation.commit_disjoint("validated rebase", [&](const auto& candidate) {
    saw_merged = candidate.relation("Left").count({7LL}) && candidate.relation("Right").count({7LL});
  });
  check(saw_merged && validated.same_revision(database.snapshot()), "validator sees the merged candidate, not the obsolete base");
  auto invalid = database.begin(); invalid.insert("Note", {99LL});
  rejects([&] { invalid.commit_disjoint("rejected", [](const auto&) { throw std::invalid_argument("invalid"); }); }, "rebase validation rejection");
  check(database.snapshot().same_revision(validated), "validator failure publishes no partial edit");

  auto retry = database.begin(); retry.insert("Right", {8LL}); int calls = 0;
  auto retried = retry.commit_disjoint("retry after concurrent commit", [&](const auto&) {
    if (++calls == 1) { auto interloper = database.begin(); interloper.insert("Left", {8LL}); interloper.commit(); }
  });
  check(calls == 2 && retried.relation("Left").count({8LL}), "revalidate after concurrent unrelated publication");
  auto limited = database.begin(); limited.insert("Right", {9LL});
  rejects([&] { limited.commit_disjoint("no retry", [&](const auto&) {
    auto interloper = database.begin(); interloper.insert("Left", {9LL}); interloper.commit();
  }, 0); }, "retry budget is enforced");
  check(!database.snapshot().relation("Right").count({9LL}), "retry exhaustion retains staged write privately");
  auto staged = database.begin(); staged.insert("Edge", {3LL, 4LL});
  check(staged.query("Reach").rows.count({1LL, 4LL}), "transaction query reads its own staged writes");
  staged.abort();
  (void)database.snapshot().query("Reach");
  auto cache_writer = database.begin(); cache_writer.insert("Note", {123LL});
  auto other_writer = database.begin(); other_writer.insert("Left", {123LL}); other_writer.commit();
  auto cached = cache_writer.commit_disjoint();
  check(cached.query("Reach").cached && cached.query("Reach").revision == cached.revision(), "rebasing retains unaffected current-head analyses with the new revision");
  auto checkpoint = database.checkpoint(); auto restore_reader = database.begin();
  (void)restore_reader.relation("Left"); restore_reader.insert("Note", {124LL});
  auto transient = database.begin(); transient.erase("Left", {123LL}); transient.commit();
  database.restore(database.snapshot(), checkpoint);
  rejects([&] { restore_reader.commit_disjoint(); }, "checkpoint restore cannot conceal an ABA dependency change");
}
ir::Liveness reference_liveness(const ir::Function& function) {
  // Independent conventional set/worklist formulation; no relational queries.
  ir::Liveness result;
  std::map<ir::Id, std::set<ir::Id>> defs, uses;
  std::map<std::pair<ir::Id, ir::Id>, std::set<ir::Id>> phi_uses;
  for (const auto& [id, block] : function.blocks) {
    for (const auto& i : block.instructions) if (ir::produces_value(i.op)) defs[id].insert(i.id);
    for (const auto& i : block.instructions) {
      if (i.op == ir::Op::Phi) {
        for (std::size_t slot = 0; slot < i.operands.size(); ++slot) phi_uses[{i.targets[slot], id}].insert(i.operands[slot]);
      } else for (auto value : i.operands) if (!defs[id].count(value)) uses[id].insert(value);
    }
    result.live_in[id]; result.live_out[id];
  }
  bool changed;
  do {
    changed = false;
    for (const auto& [id, block] : function.blocks) {
      std::set<ir::Id> out;
      for (auto target : block.instructions.back().targets) {
        auto values = result.live_in[target];
        const auto& phi = phi_uses[{id, target}]; values.insert(phi.begin(), phi.end());
        result.edges[{id, target}] = values; out.insert(values.begin(), values.end());
      }
      auto in = uses[id]; for (auto value : out) if (!defs[id].count(value)) in.insert(value);
      changed |= in != result.live_in[id] || out != result.live_out[id];
      result.live_in[id] = std::move(in); result.live_out[id] = std::move(out);
    }
  } while (changed);
  return result;
}
struct Diamond { ccdb::Snapshot snapshot; ir::Id phi, argument, condition; };
Diamond diamond(ccdb::Database& database, std::uint32_t a, std::uint32_t c, bool side_effect = false) {
  ir::Builder b(database, 1); auto entry = b.block(), left = b.block(), right = b.block(), join = b.block();
  auto condition = b.emit(entry, ir::Op::Param); b.emit(entry, ir::Op::Branch, {condition}, 0, {left, right});
  auto x = b.emit(left, ir::Op::Const, {}, a); b.emit(left, ir::Op::Jump, {}, 0, {join});
  auto y = b.emit(right, ir::Op::Const, {}, c); b.emit(right, ir::Op::Jump, {}, 0, {join});
  auto phi = b.emit(join, ir::Op::Phi, {x, y}, 0, {left, right});
  if (side_effect) { auto address = b.emit(join, ir::Op::Const); b.emit(join, ir::Op::Store, {address, phi}); }
  b.emit(join, ir::Op::Return, {phi});
  return {b.finish(), phi, x, condition};
}
void liveness() {
  std::mt19937 random(442);
  for (int trial = 0; trial < 35; ++trial) {
    ccdb::Database database(ir::program()); auto d = diamond(database, static_cast<std::uint32_t>(random()), static_cast<std::uint32_t>(random()), trial % 2);
    auto function = ir::load(d.snapshot);
    auto actual = ir::liveness(d.snapshot), reference = reference_liveness(function);
    check(actual.live_in == reference.live_in && actual.live_out == reference.live_out && actual.edges == reference.edges, "relational/worklist phi-edge liveness differential");
    check(actual.live_in.at(4).empty() && actual.before.at(d.phi).empty(), "phi inputs are edge uses, not join live-ins");
    check(actual.after.at(d.phi).count(d.phi), "parallel phi result is live after the group");
    check(actual.edges.at({2, 4}) == std::set<ir::Id>{d.argument}, "only the selected predecessor's phi operand is live on an edge");
    check(actual.revision == d.snapshot.revision(), "analysis is tagged with its coherent source revision");
  }
  ccdb::Database database(ir::program()); ir::Builder b(database, 1);
  auto entry = b.block(), header = b.block(), body = b.block(), exit = b.block();
  auto n = b.emit(entry, ir::Op::Param), zero = b.emit(entry, ir::Op::Const), one = b.emit(entry, ir::Op::Const, {}, 1);
  b.emit(entry, ir::Op::Jump, {}, 0, {header}); auto counter = b.emit(header, ir::Op::Phi);
  auto condition = b.emit(header, ir::Op::Ult, {counter, n}); b.emit(header, ir::Op::Branch, {condition}, 0, {body, exit});
  auto next = b.emit(body, ir::Op::Add, {counter, one}); b.emit(body, ir::Op::Jump, {}, 0, {header}); b.emit(exit, ir::Op::Return, {counter});
  b.incoming(counter, entry, zero); b.incoming(counter, body, next); auto source = b.finish();
  auto function = ir::load(source);
  auto actual = ir::liveness(source), reference = reference_liveness(function);
  check(actual.live_in == reference.live_in && actual.live_out == reference.live_out && actual.edges == reference.edges, "recursive liveness fixed point on a loop");
  check(actual.live_in.at(header).count(n) && actual.live_in.at(header).count(one) && !actual.live_in.at(header).count(counter), "invariant and phi loop live-ins");
  auto loops = ir::natural_loops(function);
  check(loops.size() == 1 && loops[0].header == header && loops[0].blocks == std::set<ir::Id>{header, body} &&
    loops[0].latches == std::set<ir::Id>{body} && loops[0].exits.count({header, exit}), "natural loop body, latch and exit");
  ir::Function self{0, 1, {{1, {1, {{3, ir::Op::Jump, 0, {}, {2}}}}}, {2, {2, {{4, ir::Op::Jump, 0, {}, {2}}}}}}};
  check(ir::natural_loops(self).at(0).blocks == std::set<ir::Id>{2}, "self-loop must not absorb its entry predecessor");
  rejects([&] { ir::liveness(source, {100, 0}); }, "liveness query work budget");
  auto registry = std::make_shared<speculative::Assumptions>(); auto assumption = registry->assume("trip count");
  auto version = speculative::Version::specialize(source, registry, {{n, 3, assumption}});
  auto recovery_live = ir::liveness(version.snapshot());
  check(recovery_live.revision == version.snapshot().revision(), "guarded snapshot liveness");
  ccdb::Database recovery_db(ir::program()); ir::Builder frame(recovery_db, 1); auto frame_block = frame.block();
  auto retained = frame.emit(frame_block, ir::Op::Const, {}, 17), parameter = frame.emit(frame_block, ir::Op::Param);
  frame.emit(frame_block, ir::Op::Return, {parameter}); auto baseline = frame.finish();
  auto guarded = speculative::Version::specialize(baseline, registry, {{parameter, 3, assumption}});
  check(!ir::liveness(guarded.snapshot()).before.at(parameter).count(retained), "new frames omit dead preceding values");
  // Conservative imported frames can still explicitly retain an available local.
  ccdb::Database retained_db(ir::program()); auto retained_tx = retained_db.begin();
  for (const auto& [name, rows] : guarded.snapshot().facts()) for (const auto& row : rows) retained_tx.insert(name, row);
  retained_tx.insert("Binding", {guarded.guards().front().frame.id, retained});
  auto retained_snapshot = retained_tx.commit("conservative recovery frame", [&](const auto& snapshot) { speculative::verify(baseline, snapshot); });
  check(ir::liveness(retained_snapshot).before.at(parameter).count(retained), "explicit recovery bindings remain live even without ordinary SSA users");

  ir::Function irreducible{1, 1, {
    {1, {1, {{6, ir::Op::Param, 0, {}, {}}, {7, ir::Op::Branch, 0, {6}, {2, 3}}}}},
    {2, {2, {{8, ir::Op::Branch, 0, {6}, {3, 4}}}}},
    {3, {3, {{9, ir::Op::Branch, 0, {6}, {2, 4}}}}},
    {4, {4, {{10, ir::Op::Return, 0, {6}, {}}}}}}};
  check(ir::natural_loops(irreducible).empty(), "irreducible cycles are not mislabeled as natural loops");
  ir::Function nested{0, 1, {
    {1, {1, {{8, ir::Op::Const, 1, {}, {}}, {9, ir::Op::Jump, 0, {}, {2}}}}},
    {2, {2, {{10, ir::Op::Branch, 0, {8}, {3, 7}}}}},
    {3, {3, {{11, ir::Op::Branch, 0, {8}, {4, 5}}}}},
    {4, {4, {{12, ir::Op::Jump, 0, {}, {3}}}}},
    {5, {5, {{13, ir::Op::Jump, 0, {}, {2}}}}},
    {7, {7, {{14, ir::Op::Return, 0, {8}, {}}}}}}};
  auto nesting = ir::natural_loops(nested);
  check(nesting.size() == 2 && nesting[0].blocks == std::set<ir::Id>{2, 3, 4, 5} && nesting[1].blocks == std::set<ir::Id>{3, 4}, "nested natural loops retain distinct bodies");
  auto nested_live = ir::liveness(nested), nested_reference = reference_liveness(nested);
  check(nested_live.live_in == nested_reference.live_in && nested_live.edges == nested_reference.edges, "nested-loop relational liveness matches worklists");
}
void observe_constants(const ir::Function& function, const query::Propagation& propagation, std::uint32_t argument) {
  ir::State state; state.block = function.entry; state.memory = {0};
  auto execution = ir::resume(function, {argument}, state, 10000, [&](const auto&, const auto& current) {
    check(propagation.blocks.count(current.block), "executed block must be in SCCP executable overapproximation");
    if (current.offset == 0 && current.predecessor) check(propagation.edges.count({current.predecessor, current.block}), "executed edge must be in SCCP overapproximation");
    for (const auto& [id, value] : current.values) {
      auto abstract = propagation.values.at(id);
      if (abstract.kind == query::ValueKind::Constant) check(abstract.value == value, "SCCP constant contradicted by a concrete execution");
    }
    return true;
  });
  (void)execution;
}
void sccp() {
  for (auto value : {0u, 1u, 7u, UINT32_MAX}) {
    ccdb::Database database(ir::program()); auto d = diamond(database, value, value, true); auto function = ir::load(d.snapshot);
    auto analysis = query::propagate(function);
    check(analysis.values.at(d.phi) == query::AbstractValue{query::ValueKind::Constant, value}, "distinct constant definitions meet at a phi");
    auto candidate = query::optimize(d.snapshot);
    check(fired(candidate, "conditional_constant"), "conditional constant rewrite executes");
    for (auto argument : {0u, 1u, UINT32_MAX}) {
      observe_constants(function, analysis, argument);
      check(ir::equivalent(ir::evaluate(function, {argument}, {0}), ir::evaluate(ir::load(candidate.result()), {argument}, {0})), "phi constant propagation preserves memory effects");
    }
    check(ir::evaluate(ir::load(candidate.result()), {0}).trap == "memory bounds", "constant propagation cannot delete a trapping store");
    query::Options disabled; disabled.conditional_constants = false;
    check(!fired(query::optimize(d.snapshot, disabled), "conditional_constant"), "conditional propagation can be disabled");
    query::Options no_analysis; no_analysis.analysis_work = 0;
    auto bounded = query::optimize(d.snapshot, no_analysis);
    check(bounded.budget_exhausted() && bounded.stop_reason().find("propagation") != std::string::npos, "SCCP budget returns a verified prefix");
    rejects([&] { query::propagate(function, 0); }, "partial SCCP facts are never returned as a complete analysis");
  }
  ccdb::Database varying(ir::program()); auto d = diamond(varying, 4, 9);
  auto changing = query::propagate(ir::load(d.snapshot));
  check(changing.values.at(d.phi).kind == query::ValueKind::Overdefined && changing.edges.count({3, 4}), "late executable edge widens an optimistic phi constant");
  for (auto argument : {0u, 1u}) observe_constants(ir::load(d.snapshot), changing, argument);

  ccdb::Database database(ir::program()); ir::Builder b(database, 1);
  auto entry = b.block(), header = b.block(), body = b.block(), exit = b.block();
  auto argument = b.emit(entry, ir::Op::Param), zero = b.emit(entry, ir::Op::Const), one = b.emit(entry, ir::Op::Const, {}, 1);
  auto bound = b.emit(entry, ir::Op::Const, {}, 3); b.emit(entry, ir::Op::Jump, {}, 0, {header});
  auto phi = b.emit(header, ir::Op::Phi); auto condition = b.emit(header, ir::Op::Ult, {phi, bound});
  b.emit(header, ir::Op::Branch, {condition}, 0, {body, exit});
  auto next = b.emit(body, ir::Op::Add, {phi, one}); b.emit(body, ir::Op::Jump, {}, 0, {header});
  auto result = b.emit(exit, ir::Op::Add, {phi, argument}); b.emit(exit, ir::Op::Return, {result});
  b.incoming(phi, entry, zero); b.incoming(phi, body, next); auto source = b.finish(); auto function = ir::load(source);
  auto loop = query::propagate(function);
  check(loop.values.at(phi).kind == query::ValueKind::Overdefined && loop.edges.count({header, exit}), "backedge widens a loop constant and discovers its exit");
  auto optimized = query::optimize(source);
  for (auto value : {0u, 1u, 7u, UINT32_MAX}) {
    observe_constants(function, loop, value);
    check(ir::equivalent(ir::evaluate(function, {value}), ir::evaluate(ir::load(optimized.result()), {value})), "loop-carried SCCP differential execution");
  }
  std::mt19937 random(77199);
  for (int trial = 0; trial < 25; ++trial) {
    ccdb::Database fuzz_db(ir::program()); ir::Builder build(fuzz_db, 1); auto current = build.block();
    auto input = build.emit(current, ir::Op::Param); auto value = input;
    for (int stage = 0; stage < 3; ++stage) {
      auto left = build.block(), right = build.block(), join = build.block();
      auto threshold = build.emit(current, ir::Op::Const, {}, static_cast<std::uint32_t>(random() % 7));
      auto condition = build.emit(current, random() % 2 ? ir::Op::Ult : ir::Op::Eq, {value, threshold});
      build.emit(current, ir::Op::Branch, {condition}, 0, {left, right});
      auto left_constant = build.emit(left, ir::Op::Const, {}, static_cast<std::uint32_t>(random() % 7));
      auto right_constant = build.emit(right, ir::Op::Const, {}, static_cast<std::uint32_t>(random() % 7));
      auto left_value = build.emit(left, random() % 2 ? ir::Op::Mul : ir::Op::Add, {value, left_constant});
      auto right_value = build.emit(right, random() % 2 ? ir::Op::Sub : ir::Op::Eq, {value, right_constant});
      if (random() % 3 == 0) {
        auto address = build.emit(left, ir::Op::Const, {}, static_cast<std::uint32_t>(random() % 2));
        build.emit(left, ir::Op::Store, {address, left_value});
      }
      build.emit(left, ir::Op::Jump, {}, 0, {join}); build.emit(right, ir::Op::Jump, {}, 0, {join});
      value = build.emit(join, ir::Op::Phi, {left_value, right_value}, 0, {left, right}); current = join;
    }
    build.emit(current, ir::Op::Return, {value}); auto input_snapshot = build.finish(); auto original = ir::load(input_snapshot);
    auto propagation = query::propagate(original); auto candidate = query::optimize(input_snapshot);
    auto reference = reference_liveness(original), relational = ir::liveness(input_snapshot);
    check(reference.live_in == relational.live_in && reference.edges == relational.edges, "generated CFG liveness differential");
    for (auto argument_value : {0u, 1u, 2u, UINT32_MAX, static_cast<std::uint32_t>(random())}) observe_constants(original, propagation, argument_value);
    auto report = query::validate(original, ir::load(candidate.result()), query::corpus(1, static_cast<std::uint32_t>(trial), 24, 3));
    check(report.verdict == query::Verdict::Passed, "seeded CFG/SCCP/optimizer differential trial " + std::to_string(trial) + ": " + report.explain());
  }
}
void cycles() {
  for (int size = 2; size < 9; ++size) {
    ccdb::Database database(ir::program()); ir::Builder b(database, 1);
    auto entry = b.block(), header = b.block(), body = b.block(), exit = b.block();
    auto condition = b.emit(entry, ir::Op::Param), zero = b.emit(entry, ir::Op::Const), answer = b.emit(entry, ir::Op::Const, {}, 17);
    b.emit(entry, ir::Op::Jump, {}, 0, {header}); std::vector<ir::Id> phis;
    for (int i = 0; i < size; ++i) phis.push_back(b.emit(header, ir::Op::Phi));
    b.emit(header, ir::Op::Branch, {condition}, 0, {body, exit}); b.emit(body, ir::Op::Jump, {}, 0, {header}); b.emit(exit, ir::Op::Return, {answer});
    for (int i = 0; i < size; ++i) { b.incoming(phis[i], entry, zero); b.incoming(phis[i], body, phis[(i + 1) % size]); }
    auto source = b.finish(); auto candidate = query::optimize(source);
    check(fired(candidate, "dead_pure_cycle"), "rooted dead-code elimination removes a mutually dependent phi group");
    auto before = ir::load(source), after = ir::load(candidate.result());
    check(before.blocks.size() == after.blocks.size(), "dead pure cycle elimination does not remove control-flow divergence");
    check(ir::equivalent(ir::evaluate(before, {0}), ir::evaluate(after, {0})), "dead cyclic data preserves terminating observations");
    check(ir::evaluate(before, {1}, {}, 100).trap == "step budget" && ir::evaluate(after, {1}, {}, 100).trap == "step budget", "infinite control loop remains infinite");
  }
  // An unused load remains a root because it can trap, including through a phi.
  ccdb::Database database(ir::program()); ir::Builder b(database, 1);
  auto entry = b.block(), header = b.block(), body = b.block(), exit = b.block();
  auto condition = b.emit(entry, ir::Op::Param), address = b.emit(entry, ir::Op::Const, {}, 9);
  b.emit(entry, ir::Op::Jump, {}, 0, {header}); auto phi = b.emit(header, ir::Op::Phi);
  b.emit(header, ir::Op::Load, {phi}); b.emit(header, ir::Op::Branch, {condition}, 0, {body, exit});
  b.emit(body, ir::Op::Jump, {}, 0, {header}); b.emit(exit, ir::Op::Return, {condition});
  b.incoming(phi, entry, address); b.incoming(phi, body, phi); auto source = b.finish(); auto candidate = query::optimize(source);
  check(ir::evaluate(ir::load(candidate.result()), {0}).trap == "memory bounds", "effect root keeps a trapping load and its address dependencies");
}
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("expected analysis suite");
    std::string suite = argv[1];
    if (suite == "seminaive") seminaive(); else if (suite == "transactions") transactions();
    else if (suite == "liveness") liveness(); else if (suite == "sccp") sccp(); else if (suite == "cycles") cycles();
    else throw std::invalid_argument("unknown analysis suite");
    std::cout << suite << " analysis regiment passed\n"; return 0;
  } catch (const std::exception& error) { std::cerr << "analysis failure: " << error.what() << '\n'; return 1; }
}
