#include "novostone/QueryBasedIR/QueryBasedIR.hpp"

#include <bit>
#include <locale>
#include <sstream>

namespace novostone::query {
Reduction minimize(const ir::Function& before, const ir::Function& after,
                   Input input, ReductionOptions options) {
  ir::verify(before); ir::verify(after);
  if (options.evaluations == 0) throw std::invalid_argument("reduction needs an initial evaluation");
  if (input.arguments.size() != before.arity) throw std::invalid_argument("reduction input arity mismatch");
  Reduction result;
  auto attempt = [&](Input candidate) {
    if (result.evaluations == options.evaluations) {
      result.budget_exhausted = true;
      return false;
    }
    ++result.evaluations;
    auto reference = ir::evaluate(before, candidate.arguments, candidate.memory, options.step_budget);
    auto optimized = ir::evaluate(after, candidate.arguments, candidate.memory, options.step_budget);
    if (reference.trap == "step budget" || optimized.trap == "step budget") {
      ++result.inconclusive;
      return false;
    }
    if (ir::equivalent(reference, optimized)) return false;
    result.counterexample = {std::move(candidate), std::move(reference), std::move(optimized)};
    return true;
  };
  if (!attempt(std::move(input))) throw std::invalid_argument("reduction requires a completed mismatch");

  bool changed;
  do {
    changed = false;
    // Remove only suffixes: surviving memory addresses keep their identity.
    for (auto chunk = std::bit_floor(result.counterexample.input.memory.size()); chunk; chunk /= 2) {
      while (result.counterexample.input.memory.size() >= chunk) {
        auto candidate = result.counterexample.input;
        candidate.memory.resize(candidate.memory.size() - chunk);
        if (!attempt(std::move(candidate))) break;
        changed = true;
      }
      if (result.budget_exhausted) return result;
    }
    for (bool arguments : {true, false}) {
      auto count = arguments ? result.counterexample.input.arguments.size() : result.counterexample.input.memory.size();
      for (std::size_t index = 0; index < count; ++index) {
        auto value = [&] {
          return arguments ? result.counterexample.input.arguments[index] : result.counterexample.input.memory[index];
        };
        if (value() == 0) continue;
        auto replace = [&](std::uint32_t smaller) {
          auto candidate = result.counterexample.input;
          (arguments ? candidate.arguments : candidate.memory)[index] = smaller;
          return attempt(std::move(candidate));
        };
        if (replace(0)) changed = true;
        else {
          for (int bit = 31; bit >= 0 && !result.budget_exhausted; --bit) {
            auto smaller = value() & ~(std::uint32_t{1} << bit);
            if (smaller != value() && smaller != 0 && replace(smaller)) changed = true;
          }
        }
        if (result.budget_exhausted) return result;
      }
    }
  } while (changed);
  return result;
}

std::string Reduction::explain() const {
  std::ostringstream out; out.imbue(std::locale::classic());
  out << "counterexample reduction: " << evaluations << " paired evaluations; resource-limited " << inconclusive
      << "; " << (budget_exhausted ? "evaluation budget exhausted" : "greedy reduction complete")
      << "; no global minimality guarantee\n";
  Validation witness;
  witness.verdict = Verdict::Mismatch;
  witness.counterexample = counterexample;
  out << witness.explain();
  return out.str();
}
} // namespace novostone::query
