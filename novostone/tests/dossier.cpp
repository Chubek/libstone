#include "novostone/CostCC/CostCC.hpp"
#include "novostone/SpeculativeIR/SpeculativeIR.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

using namespace novostone;
namespace {
void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F action, const std::string& message) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  check(rejected, message);
}

void recovery() {
  ccdb::Database db(ir::program()); ir::Builder b(db, 2);
  auto entry = b.block(), left = b.block(), right = b.block(), join = b.block();
  auto dead = b.emit(entry, ir::Op::Const, {}, 77);
  auto address = b.emit(entry, ir::Op::Const);
  auto loaded = b.emit(entry, ir::Op::Load, {address});
  auto x = b.emit(entry, ir::Op::Param);
  b.emit(entry, ir::Op::Store, {address, x});
  auto y = b.emit(entry, ir::Op::Param, {}, 1);
  b.emit(entry, ir::Op::Branch, {y}, 0, {left, right});
  b.emit(left, ir::Op::Jump, {}, 0, {join}); b.emit(right, ir::Op::Jump, {}, 0, {join});
  auto phi = b.emit(join, ir::Op::Phi, {loaded, x}, 0, {left, right});
  b.emit(join, ir::Op::Return, {phi});
  auto source = b.finish(); auto reference = ir::load(source);
  auto assumptions = std::make_shared<speculative::Assumptions>();
  auto assumption = assumptions->assume("entry parameters");
  auto version = speculative::Version::specialize(source, assumptions, {{x, 7, assumption}, {y, 1, assumption}});
  check(version.guards()[0].frame.locals == std::vector<ir::Id>{address, loaded}, "first frame keeps future memory operands");
  check(version.guards()[1].frame.locals == std::vector<ir::Id>{loaded, x}, "second frame keeps edge uses and drops dead locals");
  check(version.snapshot().relation("Binding").size() == 4, "compact frames have four bindings instead of seven");
  for (const auto& guard : version.guards()) {
    for (auto first : {0u, 7u, UINT32_MAX}) for (auto second : {0u, 1u, UINT32_MAX}) {
      for (std::size_t budget = 0; budget < 16; ++budget) {
        auto expected = ir::evaluate(reference, {first, second}, {33}, budget);
        auto actual = version.execute({first, second}, {33}, budget, guard.id);
        check(ir::equivalent(expected, actual.outcome), "compact recovery preserves effects, phi edges and budget");
        check(expected.state.steps == actual.outcome.state.steps, "deoptimization retains consumed steps");
      }
    }
    auto actual = version.execute({7, 1}, {33}, 100, guard.id);
    check(actual.failed_guard == guard.id && actual.outcome.value == 33 && actual.outcome.state.writes.size() == 1,
          "each forced guard resumes after exactly the effects already executed");
  }
  check(version.execute({7, 1}).outcome.trap == "memory bounds", "load traps before either guard");
  ccdb::Database malformed(ir::program()); auto tx = malformed.begin();
  for (const auto& [name, rows] : version.snapshot().facts()) for (const auto& row : rows) tx.insert(name, row);
  auto frame = version.guards()[1].frame.id;
  tx.erase("Binding", {frame, loaded});
  rejects([&] { speculative::verify(source, tx.preview()); }, "missing phi-edge recovery value rejected");
  tx.insert("Binding", {frame, loaded});
  tx.insert("Binding", {frame, phi});
  rejects([&] { speculative::verify(source, tx.preview()); }, "future recovery value rejected");
  tx.erase("Binding", {frame, phi});
  tx.insert("Binding", {frame, dead});
  speculative::verify(source, tx.preview()); // Older conservative frames remain valid.
  assumptions->invalidate(assumption);
  check(ir::equivalent(version.execute({7, 1}, {33}).outcome, ir::evaluate(reference, {7, 1}, {33})),
        "invalidation preserves compact recovery");
  ccdb::Database empty_db(ir::program()); ir::Builder empty(empty_db, 1); auto block = empty.block();
  empty.emit(block, ir::Op::Const, {}, 123); auto parameter = empty.emit(block, ir::Op::Param);
  empty.emit(block, ir::Op::Return, {parameter}); auto empty_source = empty.finish();
  auto fresh = assumptions->assume("empty frame");
  auto empty_version = speculative::Version::specialize(empty_source, assumptions, {{parameter, 3, fresh}});
  check(empty_version.guards().front().frame.locals.empty() && empty_version.execute({9}).outcome.value == 9,
        "empty recovery frame is complete when no previous value is live");
}

void uncertainty() {
  cost::Context context{"test-cpu", "scalar", "none", "latency", 9};
  std::vector<cost::Sample> training, calibration;
  std::vector<cost::Features> vectors{{0,0,0,0}, {10,0,0,0}, {0,10,0,0}, {0,0,10,0}, {0,0,0,10}, {10,10,10,10}};
  auto latency = [](const cost::Features& f) { return 1000 - f[0] + 5*f[1] + 8*f[2] + 3*f[3]; };
  for (std::size_t i = 0; i < vectors.size(); ++i)
    training.push_back({context, "train-" + std::to_string(i), vectors[i], latency(vectors[i])});
  for (auto f : {cost::Features{2,3,4,5}, cost::Features{5,4,3,2}})
    calibration.push_back({context, "held-out", f, latency(f)});
  auto exact = cost::Model::train("exact", training, calibration);
  check(exact.calibration()->training_samples == 6 && exact.calibration()->calibration_samples == 2 &&
        exact.calibration()->training_workloads == 6 && exact.calibration()->calibration_workloads == 1,
        "sample counts distinguish repeated observations from independent workloads");
  auto noisy_samples = calibration; noisy_samples[0].latency += 4; noisy_samples[1].latency -= 4;
  auto noisy = cost::Model::train("noisy", training, noisy_samples);
  check(noisy.calibration()->max_absolute_error > 3.99, "held-out residuals determine the empirical envelope");
  for (const auto& sample : noisy_samples) {
    auto prediction = noisy.predict(context, sample.features);
    check(prediction->interval && prediction->interval->lower <= sample.latency && prediction->interval->upper >= sample.latency,
          "calibration measurements fit the empirical envelope");
  }
  ccdb::Database db(ir::program()); ir::Builder b(db, 1); auto block = b.block();
  auto x = b.emit(block, ir::Op::Param), zero = b.emit(block, ir::Op::Const);
  auto sum = b.emit(block, ir::Op::Add, {x, zero}); b.emit(block, ir::Op::Return, {sum});
  auto source = b.finish();
  std::vector<query::Candidate> candidates{query::baseline(source), query::optimize(source)};
  cost::Ranker certain(exact), uncertain(noisy);
  auto precise = certain.choose(candidates, context), overlapping = uncertain.choose(candidates, context);
  check(precise.used_model && precise.selected == 0, "separated envelopes can override the heuristic");
  check(overlapping.candidates[0].prediction->confidence > 0.99 && !overlapping.used_model && overlapping.selected == 1,
        "high aggregate confidence cannot justify a choice inside the measured error");
  check(overlapping.reason.find("overlapping") != std::string::npos && overlapping.explain().find("held_out_workloads 1") != std::string::npos,
        "decision log explains uncertainty and evidence provenance");
  std::reverse(candidates.begin(), candidates.end());
  check(uncertain.choose(candidates, context).selected == 0, "fallback resets a previously selected model winner");
  check(!certain.choose({candidates[0], candidates[0]}, context).used_model, "tied predictions use deterministic fallback");
  auto restored = cost::Model::parse(noisy.serialize());
  check(restored.serialize() == noisy.serialize(), "calibration evidence round-trips at full precision");
  auto prediction = restored.predict(context, {0,0,0,0});
  check(prediction->interval->lower >= 0, "prediction lower bounds are nonnegative");
  auto legacy = exact.serialize(); legacy.replace(0, std::string("novostone-cost 2").size(), "novostone-cost 1");
  auto start = legacy.find("evidence "), end = legacy.find('\n', start);
  legacy.erase(start, end - start + 1);
  auto old = cost::Model::parse(legacy);
  check(!old.calibration() && !old.predict(context, {3,0,0,1})->interval, "v1 models do not invent calibration evidence");
  cost::Ranker old_ranker(old);
  check(!old_ranker.choose(candidates, context).used_model, "legacy model without evidence falls back");
  check(!cost::Model::parse(old.serialize()).calibration(), "v1 migration preserves missing evidence");
  for (const auto& record : {"evidence -1 2 1 1 0", "evidence 6 2 7 1 0", "evidence 6 2 1 0 0",
                             "evidence 6 2 1 1 -1", "evidence 6 2 1 1 nan", "evidence 6 2 1 1 1e999",
                             "evidence 18446744073709551616 2 1 1 0", "evidence 6 1 1 1 0"}) {
    auto malformed = noisy.serialize(); auto first = malformed.find("evidence "), last = malformed.find('\n', first);
    malformed.replace(first, last - first, record);
    rejects([&] { cost::Model::parse(malformed); }, "malformed calibration evidence rejected");
  }
  auto stale = context; ++stale.profile_epoch;
  check(!uncertain.choose(candidates, stale).used_model, "uncertainty evidence remains scoped to profile epoch");
}

void reduction() {
  ir::Function identity{1, 1, {{1, {1, {{2, ir::Op::Param, 0, {}, {}}, {3, ir::Op::Return, 0, {2}, {}}}}}}};
  auto zero = identity; zero.blocks.at(1).instructions[0].op = ir::Op::Const;
  query::Input initial{{UINT32_MAX}, std::vector<std::uint32_t>(20, UINT32_MAX)};
  auto reduced = query::minimize(identity, zero, initial);
  check(reduced.counterexample.input.arguments == std::vector<std::uint32_t>{1} && reduced.counterexample.input.memory.empty(),
        "integer and memory reduction retains the smallest nonzero witness");
  check(!reduced.budget_exhausted && reduced.evaluations <= 256, "greedy reduction completes within its paired execution budget");
  auto repeat = query::minimize(identity, zero, initial);
  check(repeat.explain() == reduced.explain(), "counterexample reduction is deterministic");
  check(query::validate(identity, zero, {reduced.counterexample.input}).verdict == query::Verdict::Mismatch,
        "reported reduced witness reproduces independently");
  for (std::size_t budget = 1; budget < 12; ++budget) {
    auto bounded = query::minimize(identity, zero, initial, {budget, 100});
    check(bounded.budget_exhausted && bounded.evaluations == budget &&
          query::validate(identity, zero, {bounded.counterexample.input}).verdict == query::Verdict::Mismatch,
          "every exhausted reduction prefix retains a verified counterexample");
  }
  rejects([&] { query::minimize(identity, zero, initial, {0, 100}); }, "zero evaluations cannot certify the initial witness");
  rejects([&] { query::minimize(identity, zero, initial, {10, 0}); }, "initial resource exhaustion is not a counterexample");
  rejects([&] { query::minimize(identity, identity, initial); }, "equivalent input cannot be minimized as a mismatch");
  rejects([&] { query::minimize(identity, zero, {{}, {}}); }, "wrong-arity reduction input rejected");

  // Both variants trap on empty memory; only the ordered write trace differs
  // on the final reduced input, where the store writes the existing value.
  ir::Function effect{0, 1, {{1, {1, {{2, ir::Op::Const, 0, {}, {}}, {3, ir::Op::Load, 0, {2}, {}},
    {4, ir::Op::Store, 0, {2, 2}, {}}, {5, ir::Op::Return, 0, {2}, {}}}}}}};
  auto missing = effect; missing.blocks.at(1).instructions.erase(missing.blocks.at(1).instructions.begin() + 2);
  auto writes = query::minimize(effect, missing, {{}, {97, 98, 99}});
  check(writes.counterexample.input.memory == std::vector<std::uint32_t>{0}, "memory shrink preserves required valid address");
  check(writes.counterexample.before.value == writes.counterexample.after.value &&
        writes.counterexample.before.state.memory == writes.counterexample.after.state.memory &&
        writes.counterexample.before.state.writes != writes.counterexample.after.state.writes, "ordered effects remain part of the reduction oracle");

  ir::Function branch{1, 1, {
    {1, {1, {{4, ir::Op::Param, 0, {}, {}}, {5, ir::Op::Branch, 0, {4}, {2,3}}}}},
    {2, {2, {{6, ir::Op::Const, 1, {}, {}}, {7, ir::Op::Return, 0, {6}, {}}}}},
    {3, {3, {{8, ir::Op::Jump, 0, {}, {3}}}}}}};
  auto mutant = branch; mutant.blocks.at(2).instructions[0].immediate = 2;
  auto timeout = query::minimize(branch, mutant, {{9}, {}}, {100, 20});
  check(timeout.inconclusive > 0 && timeout.counterexample.input.arguments == std::vector<std::uint32_t>{1},
        "timed-out reductions are rejected without losing the previous witness");
  check(timeout.counterexample.before.trap.empty() && timeout.counterexample.after.trap.empty(), "final witness completed both executions");
}
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::invalid_argument("expected dossier suite");
    std::string suite = argv[1];
    if (suite == "recovery") recovery(); else if (suite == "uncertainty") uncertainty(); else if (suite == "reduction") reduction();
    else throw std::invalid_argument("unknown dossier suite");
    std::cout << suite << " dossier contracts passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
