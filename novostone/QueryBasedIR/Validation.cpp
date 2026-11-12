#include "novostone/QueryBasedIR/QueryBasedIR.hpp"

#include <algorithm>
#include <random>
#include <sstream>

namespace novostone::query {
Validation validate(const ir::Function& before, const ir::Function& after,
                    const std::vector<Input>& inputs, std::size_t budget) {
  ir::verify(before); ir::verify(after);
  if (inputs.empty()) throw std::invalid_argument("validation needs a nonempty input domain");
  Validation report;
  for (const auto& input : inputs) {
    if (input.arguments.size() != before.arity) throw std::invalid_argument("validation corpus arity mismatch");
    auto reference = ir::evaluate(before, input.arguments, input.memory, budget);
    auto candidate = ir::evaluate(after, input.arguments, input.memory, budget);
    if (reference.trap == "step budget" || candidate.trap == "step budget") { ++report.inconclusive; continue; }
    if (!ir::equivalent(reference, candidate)) {
      report.verdict = Verdict::Mismatch;
      report.counterexample = Counterexample{input, std::move(reference), std::move(candidate)};
      return report;
    }
    ++report.matched;
  }
  report.verdict = report.inconclusive ? Verdict::Inconclusive : Verdict::Passed;
  return report;
}
std::vector<Input> corpus(std::uint32_t arity, std::uint32_t seed, std::size_t samples, std::size_t max_memory) {
  // Explicit caps prevent accidental multi-gigabyte test domains.
  if (arity > 1024 || samples > 100000 || max_memory > 65536 ||
      (static_cast<std::uint64_t>(arity) + max_memory) * samples > 10000000)
    throw std::invalid_argument("validation corpus size limit exceeded");
  std::mt19937 random(seed);
  const std::vector<std::uint32_t> edges{0, 1, 2, 0x7fffffff, 0x80000000, UINT32_MAX};
  std::vector<Input> inputs;
  for (std::size_t sample = 0; sample < samples; ++sample) {
    Input input;
    for (std::uint32_t argument = 0; argument < arity; ++argument)
      input.arguments.push_back(sample < edges.size() ? edges[(sample + argument) % edges.size()] : static_cast<std::uint32_t>(random()));
    auto cells = sample % (max_memory + 1);
    for (std::size_t cell = 0; cell < cells; ++cell) input.memory.push_back(static_cast<std::uint32_t>(random()));
    inputs.push_back(std::move(input));
  }
  return inputs;
}
ccdb::Snapshot apply_checked(ccdb::Database& database, const Candidate& candidate,
                            const std::vector<Input>& inputs, std::size_t budget) {
  auto report = validate(ir::load(candidate.base()), ir::load(candidate.result()), inputs, budget);
  if (report.verdict != Verdict::Passed) throw std::runtime_error("candidate not published: " + report.explain());
  return apply(database, candidate);
}
std::string Validation::explain() const {
  std::ostringstream out;
  out << "bounded differential validation: " << (verdict == Verdict::Passed ? "passed" : verdict == Verdict::Mismatch ? "mismatch" : "inconclusive")
      << "; matched " << matched << "; resource-limited " << inconclusive << "; scope: supplied finite inputs, i32 and ordered memory effects";
  if (counterexample) {
    out << "\narguments";
    for (auto value : counterexample->input.arguments) out << ' ' << value;
    out << "\nmemory";
    for (auto value : counterexample->input.memory) out << ' ' << value;
    auto outcome = [&](const char* label, const ir::Outcome& result) {
      out << '\n' << label << " value ";
      if (result.value) out << *result.value; else out << "none";
      out << " trap " << result.trap << " writes";
      for (const auto& write : result.state.writes) out << " [" << write.address << "]=" << write.value;
      out << " memory";
      for (auto value : result.state.memory) out << ' ' << value;
    };
    outcome("before", counterexample->before); outcome("after", counterexample->after);
  }
  return out.str();
}
} // namespace novostone::query
