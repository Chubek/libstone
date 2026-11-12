#include "novostone/CostCC/CostCC.hpp"
#include "novostone/SpeculativeIR/SpeculativeIR.hpp"

#include <fstream>
#include <iostream>
#include <iterator>
#include <charconv>

using namespace novostone;
namespace {
ir::Function read_function(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open input IR: " + path);
  std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  return ir::parse(text);
}
std::uint32_t integer(const std::string& text) {
  std::uint32_t value;
  auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) throw std::invalid_argument("expected unsigned i32: " + text);
  return value;
}
}
int main(int argc, char** argv) {
  try {
    ccdb::Database db(ir::program(), [](const auto& s) { ir::verify(s); });
    if (argc == 3 && std::string(argv[1]) == "analyze") {
      auto function = read_function(argv[2]);
      auto source = ir::publish(db, db.snapshot(), function, "analysis input");
      std::cout << ir::liveness(source).explain() << query::propagate(function).explain();
      for (const auto& loop : ir::natural_loops(function)) {
        std::cout << "loop header " << loop.header << " blocks";
        for (auto block : loop.blocks) std::cout << ' ' << block;
        std::cout << " latches";
        for (auto latch : loop.latches) std::cout << ' ' << latch;
        std::cout << '\n';
      }
      return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "run") {
      auto function = read_function(argv[2]);
      std::vector<std::uint32_t> arguments;
      for (int i = 3; i < argc; ++i) arguments.push_back(integer(argv[i]));
      auto outcome = ir::evaluate(function, arguments);
      if (!outcome.trap.empty()) { std::cerr << "trap: " << outcome.trap << '\n'; return 1; }
      std::cout << "result " << outcome.value.value() << " steps " << outcome.state.steps << '\n';
      return 0;
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "validate") {
      auto before = read_function(argv[2]), after = read_function(argv[3]);
      auto seed = argc == 5 ? integer(argv[4]) : 728u;
      auto report = query::validate(before, after, query::corpus(before.arity, seed));
      std::cout << "seed " << seed << '\n' << report.explain() << '\n';
      return report.verdict == query::Verdict::Passed ? 0 : report.verdict == query::Verdict::Mismatch ? 1 : 2;
    }
    if (argc == 3 && std::string(argv[1]) == "optimize") {
      auto source = ir::publish(db, db.snapshot(), read_function(argv[2]), "parse input");
      auto candidate = query::optimize(source);
      std::cout << ir::dump(ir::load(candidate.result()));
      std::cerr << candidate.explain() << '\n';
      return 0;
    }
    if (argc != 1 && !(argc == 2 && std::string(argv[1]) == "demo")) {
      std::cerr << "usage: novostone [demo | analyze FILE | optimize FILE | run FILE [I32...] | validate BEFORE AFTER [SEED]]\n";
      return 2;
    }
    ir::Builder builder(db, 1);
    auto block = builder.block();
    auto x = builder.emit(block, ir::Op::Param, {}, 0);
    auto zero = builder.emit(block, ir::Op::Const, {}, 0);
    auto sum = builder.emit(block, ir::Op::Add, {x, zero});
    builder.emit(block, ir::Op::Return, {sum});
    auto source = builder.finish();
    auto optimized = query::optimize(source);
    cost::Ranker ranker;
    std::vector<query::Candidate> candidates{query::baseline(source), optimized};
    auto decision = ranker.choose(candidates, {"reference-i32", "scalar", "none", "demo", 1});
    std::cout << optimized.explain() << '\n' << decision.explain();
    auto assumptions = std::make_shared<speculative::Assumptions>();
    auto assumption = assumptions->assume("argument usually equals 42");
    auto version = speculative::Version::specialize(source, assumptions, {{x, 42, assumption}});
    for (auto value : {42u, 7u}) {
      auto execution = version.execute({value});
      std::cout << "input " << value << " result " << execution.outcome.value.value()
                << " deoptimized " << execution.failed_guard.has_value() << '\n';
    }
    assumptions->invalidate(assumption);
    std::cout << "after invalidation: " << version.execute({42}).reason << '\n';
    query::apply(db, candidates[decision.selected]);
    std::cout << db.snapshot().dump();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "novostone: " << error.what() << '\n';
    return 1;
  }
}
