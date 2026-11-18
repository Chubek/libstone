#include "novostone/PCC/SMTProof.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <locale>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#if defined(NOVOSTONE_PCC_USE_SATIE) && NOVOSTONE_PCC_USE_SATIE
#include "SatieFrontendSMTLIB2.hpp"
#endif

namespace novostone::pcc {
namespace {

using Word = std::array<std::string, 32>;

constexpr std::string_view kLogic = "QF_UF";
constexpr std::string_view kEncoding = "i32-bool-bitblast-v1";
constexpr std::string_view kCongruenceEncoding =
    "i32-bool-congruence-v1";
constexpr std::size_t kMaximumGates = 100000;
constexpr std::size_t kMaximumProofBytes = 64 * 1024 * 1024;

std::string bit_suffix(std::size_t bit) {
  if (bit >= 100) throw std::logic_error("bit index exceeds identifier format");
  std::string result;
  result.push_back(static_cast<char>('0' + bit / 10));
  result.push_back(static_cast<char>('0' + bit % 10));
  return result;
}

std::string gate_suffix(std::size_t gate) {
  auto value = std::to_string(gate);
  if (value.size() < 6) value.insert(value.begin(), 6 - value.size(), '0');
  return value;
}

bool same_instruction(const ir::Instruction& left, const ir::Instruction& right) {
  return left.id == right.id && left.op == right.op &&
         left.immediate == right.immediate && left.operands == right.operands &&
         left.targets == right.targets;
}

bool same_function(const ir::Function& left, const ir::Function& right) {
  if (left.arity != right.arity || left.entry != right.entry ||
      left.blocks.size() != right.blocks.size())
    return false;
  auto a = left.blocks.begin();
  auto b = right.blocks.begin();
  for (; a != left.blocks.end(); ++a, ++b) {
    if (a->first != b->first || a->second.id != b->second.id ||
        a->second.instructions.size() != b->second.instructions.size())
      return false;
    for (std::size_t index = 0; index < a->second.instructions.size(); ++index)
      if (!same_instruction(a->second.instructions[index],
                            b->second.instructions[index]))
        return false;
  }
  return true;
}

struct InstructionLocation {
  ir::Id block = 0;
  std::size_t index = 0;
};

InstructionLocation locate(const ir::Function& function, ir::Id instruction) {
  for (const auto& [block_id, block] : function.blocks)
    for (std::size_t index = 0; index < block.instructions.size(); ++index)
      if (block.instructions[index].id == instruction)
        return {block_id, index};
  throw std::invalid_argument("rewrite refers to a missing instruction");
}

const ir::Instruction& instruction_at(const ir::Function& function,
                                      InstructionLocation location) {
  return function.blocks.at(location.block).instructions.at(location.index);
}

ir::Instruction& instruction_at(ir::Function& function,
                                InstructionLocation location) {
  return function.blocks.at(location.block).instructions.at(location.index);
}

const ir::Instruction& definition(const ir::Function& function, ir::Id value) {
  return instruction_at(function, locate(function, value));
}

bool constant_value(const ir::Function& function, ir::Id value,
                    std::uint32_t& result) {
  const auto& defining = definition(function, value);
  if (defining.op != ir::Op::Const) return false;
  result = defining.immediate;
  return true;
}

bool is_constant(const ir::Function& function, ir::Id value,
                 std::uint32_t expected) {
  std::uint32_t actual = 0;
  return constant_value(function, value, actual) && actual == expected;
}

void make_constant(ir::Instruction& instruction, std::uint32_t value) {
  instruction.op = ir::Op::Const;
  instruction.immediate = value;
  instruction.operands.clear();
  instruction.targets.clear();
}

void make_copy(ir::Instruction& instruction, ir::Id value) {
  instruction.op = ir::Op::Copy;
  instruction.immediate = 0;
  instruction.operands = {value};
  instruction.targets.clear();
}

void replace_uses(ir::Function& function, ir::Id from, ir::Id to) {
  for (auto& [block_id, block] : function.blocks) {
    (void)block_id;
    for (auto& instruction : block.instructions)
      for (auto& operand : instruction.operands)
        if (operand == from) operand = to;
  }
}

void erase_instruction(ir::Function& function, InstructionLocation location) {
  auto& instructions = function.blocks.at(location.block).instructions;
  instructions.erase(instructions.begin() +
                     static_cast<std::ptrdiff_t>(location.index));
}

void require_exact_result(const ir::Function& expected,
                          const ir::Function& actual,
                          std::string_view rule) {
  if (!same_function(expected, actual))
    throw std::invalid_argument("rewrite record '" + std::string(rule) +
                                "' does not match the exact before/after IR");
}

bool commutative(ir::Op op) {
  return op == ir::Op::Add || op == ir::Op::Mul || op == ir::Op::Eq;
}

std::vector<ir::Id> normalized_operands(const ir::Instruction& instruction) {
  auto operands = instruction.operands;
  if (commutative(instruction.op))
    std::sort(operands.begin(), operands.end());
  return operands;
}

bool same_expression(const ir::Instruction& left,
                     const ir::Instruction& right) {
  return left.op == right.op && left.immediate == right.immediate &&
         normalized_operands(left) == normalized_operands(right);
}

class BooleanCircuit {
public:
  explicit BooleanCircuit(bool simplify = true) : simplify_(simplify) {}

  Word input_word(const std::string& prefix) {
    Word result;
    for (std::size_t bit = 0; bit < result.size(); ++bit) {
      result[bit] = prefix + "_b" + bit_suffix(bit);
      input_bit(result[bit]);
    }
    return result;
  }

  static Word constant(std::uint32_t value) {
    Word result;
    for (std::size_t bit = 0; bit < result.size(); ++bit)
      result[bit] = (value & (std::uint32_t{1} << bit)) ? "true" : "false";
    return result;
  }

  Word add(const Word& left, const Word& right,
           const std::string& carry_in = "false") {
    Word result;
    std::string carry = carry_in;
    for (std::size_t bit = 0; bit < result.size(); ++bit) {
      auto pair = xor_bit(left[bit], right[bit]);
      result[bit] = xor_bit(pair, carry);
      auto both = and_bit(left[bit], right[bit]);
      auto propagated = and_bit(carry, pair);
      carry = or_bit(both, propagated);
    }
    return result;
  }

  Word subtract(const Word& left, const Word& right) {
    Word complement;
    for (std::size_t bit = 0; bit < complement.size(); ++bit)
      complement[bit] = not_bit(right[bit]);
    return add(left, complement, "true");
  }

  Word multiply(Word left, Word right) {
    const auto left_constant = as_constant(left);
    const auto right_constant = as_constant(right);
    if (left_constant &&
        (!right_constant ||
         std::popcount(*left_constant) < std::popcount(*right_constant)))
      std::swap(left, right);

    Word result = constant(0);
    for (std::size_t shift = 0; shift < 32; ++shift) {
      if (right[shift] == "false") continue;
      Word partial = constant(0);
      for (std::size_t bit = shift; bit < 32; ++bit) {
        const auto source = bit - shift;
        partial[bit] = right[shift] == "true"
                           ? left[source]
                           : and_bit(left[source], right[shift]);
      }
      result = add(result, partial);
    }
    return result;
  }

  Word equal_word(const Word& left, const Word& right) {
    std::string equal = "true";
    for (std::size_t bit = 0; bit < 32; ++bit)
      equal = and_bit(equal, iff_bit(left[bit], right[bit]));
    Word result = constant(0);
    result[0] = std::move(equal);
    return result;
  }

  Word less_unsigned(const Word& left, const Word& right) {
    std::string less = "false";
    std::string equal = "true";
    for (std::size_t bit = 32; bit-- > 0;) {
      auto left_zero = not_bit(left[bit]);
      auto zero_one = and_bit(left_zero, right[bit]);
      auto newly_less = and_bit(equal, zero_one);
      less = or_bit(less, newly_less);
      equal = and_bit(equal, iff_bit(left[bit], right[bit]));
    }
    Word result = constant(0);
    result[0] = std::move(less);
    return result;
  }

  std::string nonzero(const Word& condition) {
    std::string result = "false";
    for (const auto& bit : condition)
      result = or_bit(result, bit);
    return result;
  }

  Word select(const Word& condition, const Word& yes, const Word& no) {
    return choose(nonzero(condition), yes, no);
  }

  Word choose(const std::string& condition, const Word& yes, const Word& no) {
    auto zero = not_bit(condition);
    Word result;
    for (std::size_t bit = 0; bit < result.size(); ++bit) {
      auto take_yes = and_bit(condition, yes[bit]);
      auto take_no = and_bit(zero, no[bit]);
      result[bit] = or_bit(take_yes, take_no);
    }
    return result;
  }

  std::string input_bit(std::string identifier) {
    inputs_.insert(identifier);
    if (inputs_.size() > kMaximumGates)
      throw std::length_error("SMT Boolean circuit input limit exceeded");
    return identifier;
  }

  std::string observe(std::string expression) {
    return gate(std::move(expression));
  }

  void violation(std::string expression) {
    violations_.push_back(gate(std::move(expression)));
  }

  static std::string differs(const Word& original, const Word& replacement) {
    std::string equality = "(not (and";
    for (std::size_t bit = 0; bit < 32; ++bit)
      equality += " (= " + original[bit] + ' ' + replacement[bit] + ')';
    return equality + "))";
  }

  std::string render(const query::Rewrite& rewrite, std::string_view claim,
                     std::string_view assertion = {}) const {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "; Novostone PCC proof obligation\n"
        << "; rule " << rewrite.rule << "\n"
        << "; instruction " << rewrite.instruction << "\n"
        << "; validity claim: " << claim << "\n"
        << "(set-logic " << kLogic << ")\n"
        << "(set-info :status unsat)\n";
    for (const auto& input : inputs_)
      out << "(declare-const " << input << " Bool)\n";
    for (const auto& gate : gates_)
      out << "(declare-const " << gate.first << " Bool)\n";
    if (!gates_.empty()) {
      out << "(assert ";
      render_gate_conjunction(out, 0, gates_.size());
      out << ")\n";
    }
    out << "(assert ";
    if (!assertion.empty()) {
      out << assertion;
    } else {
      if (violations_.empty())
        throw std::logic_error("empty SMT counterexample query");
      render_disjunction(out, 0, violations_.size());
    }
    out << ")\n(check-sat)\n";
    return out.str();
  }

private:
  void render_disjunction(std::ostream& out, std::size_t begin,
                          std::size_t end) const {
    if (end - begin == 1) {
      out << violations_[begin];
      return;
    }
    const auto middle = begin + (end - begin) / 2;
    out << "(or ";
    render_disjunction(out, begin, middle);
    out << ' ';
    render_disjunction(out, middle, end);
    out << ')';
  }

  void render_gate_conjunction(std::ostream& out, std::size_t begin,
                               std::size_t end) const {
    if (end - begin == 1) {
      out << "(= " << gates_[begin].first << ' ' << gates_[begin].second
          << ')';
      return;
    }
    const auto middle = begin + (end - begin) / 2;
    out << "(and ";
    render_gate_conjunction(out, begin, middle);
    out << ' ';
    render_gate_conjunction(out, middle, end);
    out << ')';
  }

  static std::optional<std::uint32_t> as_constant(const Word& word) {
    std::uint32_t result = 0;
    for (std::size_t bit = 0; bit < word.size(); ++bit) {
      if (word[bit] == "true")
        result |= std::uint32_t{1} << bit;
      else if (word[bit] != "false")
        return std::nullopt;
    }
    return result;
  }

  std::string gate(std::string expression) {
    if (gates_.size() >= kMaximumGates)
      throw std::length_error("SMT Boolean circuit gate limit exceeded");
    auto identifier = "g_" + gate_suffix(gates_.size());
    gates_.emplace_back(identifier, std::move(expression));
    return identifier;
  }

  std::string not_bit(const std::string& value) {
    if (simplify_) {
      if (value == "true") return "false";
      if (value == "false") return "true";
    }
    return gate("(not " + value + ")");
  }

  std::string and_bit(const std::string& left, const std::string& right) {
    if (simplify_) {
      if (left == "false" || right == "false") return "false";
      if (left == "true") return right;
      if (right == "true" || left == right) return left;
    }
    return gate("(and " + left + ' ' + right + ")");
  }

  std::string or_bit(const std::string& left, const std::string& right) {
    if (simplify_) {
      if (left == "true" || right == "true") return "true";
      if (left == "false") return right;
      if (right == "false" || left == right) return left;
    }
    return gate("(or " + left + ' ' + right + ")");
  }

  std::string xor_bit(const std::string& left, const std::string& right) {
    if (simplify_) {
      if (left == right) return "false";
      if (left == "false") return right;
      if (right == "false") return left;
      if (left == "true") return not_bit(right);
      if (right == "true") return not_bit(left);
    }
    return gate("(not (= " + left + ' ' + right + "))");
  }

  std::string iff_bit(const std::string& left, const std::string& right) {
    if (simplify_) {
      if (left == right) return "true";
      if (left == "true") return right;
      if (right == "true") return left;
      if (left == "false") return not_bit(right);
      if (right == "false") return not_bit(left);
    }
    return gate("(= " + left + ' ' + right + ")");
  }

  bool simplify_ = true;
  std::set<std::string> inputs_;
  std::vector<std::pair<std::string, std::string>> gates_;
  std::vector<std::string> violations_;
};

class ValueWords {
public:
  ValueWords(const ir::Function& function, BooleanCircuit& circuit,
             const query::Propagation* propagation = nullptr)
      : function_(function), circuit_(circuit), propagation_(propagation) {}

  Word value(ir::Id id) {
    if (propagation_) {
      const auto& fact = propagation_->values.at(id);
      if (fact.kind == query::ValueKind::Constant)
        return BooleanCircuit::constant(fact.value);
      if (fact.kind != query::ValueKind::Overdefined)
        throw std::invalid_argument("SCCP premise unavailable on executable edge");
      return circuit_.input_word("ssa_" + std::to_string(id));
    }
    const auto& defining = definition(function_, id);
    if (defining.op == ir::Op::Const)
      return BooleanCircuit::constant(defining.immediate);
    if (defining.op == ir::Op::Param)
      return circuit_.input_word("arg_" + std::to_string(defining.immediate));
    return circuit_.input_word("ssa_" + std::to_string(id));
  }

  Word expression(const ir::Instruction& instruction) {
    auto operands = normalized_operands(instruction);
    auto operand = [&](std::size_t index) {
      return value(operands.at(index));
    };
    switch (instruction.op) {
      case ir::Op::Param:
        return circuit_.input_word("arg_" +
                                   std::to_string(instruction.immediate));
      case ir::Op::Const:
        return BooleanCircuit::constant(instruction.immediate);
      case ir::Op::Copy:
        return operand(0);
      case ir::Op::Add:
        return circuit_.add(operand(0), operand(1));
      case ir::Op::Sub:
        return circuit_.subtract(operand(0), operand(1));
      case ir::Op::Mul:
        return circuit_.multiply(operand(0), operand(1));
      case ir::Op::Eq:
        return circuit_.equal_word(operand(0), operand(1));
      case ir::Op::Ult:
        return circuit_.less_unsigned(operand(0), operand(1));
      case ir::Op::Select:
        return circuit_.select(operand(0), operand(1), operand(2));
      default:
        throw std::invalid_argument(
            "rewrite expression has no sound local i32 SMT encoding");
    }
  }

private:
  const ir::Function& function_;
  BooleanCircuit& circuit_;
  const query::Propagation* propagation_;
};

SMTProofObligation finish_obligation(const query::Rewrite& rewrite,
                                     std::size_t ordinal,
                                     const BooleanCircuit& circuit,
                                     std::string claim,
                                     std::string_view encoding,
                                     std::string assertion = {}) {
  SMTProofObligation result;
  result.ordinal = ordinal;
  result.revision = rewrite.revision;
  result.instruction = rewrite.instruction;
  result.rule = rewrite.rule;
  result.identifier =
      smt_identifier(rewrite.rule + ":" +
                     std::to_string(rewrite.instruction) + ":" +
                     std::to_string(ordinal));
  result.claim = std::move(claim);
  result.logic = std::string(kLogic);
  result.encoding = std::string(encoding);
  result.smtlib2 = circuit.render(rewrite, result.claim, assertion);
  if (result.smtlib2.size() > kMaximumProofBytes)
    throw std::length_error("SMT obligation byte limit exceeded");
  return result;
}

SMTProofObligation finish_value_obligation(
    const query::Rewrite& rewrite, std::size_t ordinal,
    const Word& original, const Word& replacement, const BooleanCircuit& circuit,
    std::string claim, std::string_view encoding = kEncoding) {
  return finish_obligation(rewrite, ordinal, circuit, std::move(claim),
                           encoding, BooleanCircuit::differs(original, replacement));
}

std::uint32_t folded_value(ir::Op op, std::uint32_t left,
                           std::uint32_t right) {
  switch (op) {
    case ir::Op::Add:
      return static_cast<std::uint32_t>(std::uint64_t{left} + right);
    case ir::Op::Sub:
      return static_cast<std::uint32_t>(std::uint64_t{left} - right);
    case ir::Op::Mul:
      return static_cast<std::uint32_t>(std::uint64_t{left} * right);
    case ir::Op::Eq:
      return left == right ? 1u : 0u;
    case ir::Op::Ult:
      return left < right ? 1u : 0u;
    default:
      throw std::invalid_argument("constant-fold record has a non-foldable opcode");
  }
}

bool is_fold_rule(std::string_view rule) {
  return rule == "fold_add" || rule == "fold_sub" ||
         rule == "fold_mul" || rule == "fold_eq" ||
         rule == "fold_ult";
}

bool is_local_replacement_rule(std::string_view rule) {
  return rule == "add_zero" || rule == "sub_zero" ||
         rule == "mul_one" || rule == "sub_self" ||
         rule == "eq_self" || rule == "ult_self" ||
         rule == "mul_zero" || rule == "select_value";
}

bool is_known_rule(std::string_view rule) {
  return is_fold_rule(rule) || is_local_replacement_rule(rule) ||
         rule == "local_cse" || rule == "copy_propagation" ||
         rule == "phi_uniform" || rule == "fold_branch" ||
         rule == "forward_load" || rule == "dead_pure" ||
         rule == "dead_pure_cycle" || rule == "conditional_constant";
}

SMTProofObligation emit_fold(const ir::Function& before,
                             const ir::Function& after,
                             const query::Rewrite& rewrite,
                             std::size_t ordinal) {
  const auto location = locate(before, rewrite.instruction);
  const auto& original_instruction = instruction_at(before, location);
  const auto expected_rule = "fold_" + ir::name(original_instruction.op);
  if (rewrite.rule != expected_rule || original_instruction.operands.size() != 2)
    throw std::invalid_argument(
        "constant-fold rewrite rule does not match its instruction");

  std::uint32_t left = 0;
  std::uint32_t right = 0;
  if (!constant_value(before, original_instruction.operands[0], left) ||
      !constant_value(before, original_instruction.operands[1], right))
    throw std::invalid_argument(
        "constant-fold rewrite operands are not direct i32 constants");
  const auto result_value =
      folded_value(original_instruction.op, left, right);

  auto expected = before;
  make_constant(instruction_at(expected, location), result_value);
  require_exact_result(expected, after, rewrite.rule);

  // Keep the original operation visible as a gate-level modular-i32
  // expression instead of reducing both sides to precomputed bit literals.
  BooleanCircuit circuit(false);
  ValueWords words(before, circuit);
  const auto original = words.expression(original_instruction);
  const auto replacement = BooleanCircuit::constant(result_value);
  return finish_value_obligation(
      rewrite, ordinal, original, replacement, circuit,
      "the folded constant equals the original modular i32 expression");
}

ir::Id expected_copy_target(const ir::Function& before,
                            const ir::Instruction& instruction,
                            std::string_view rule,
                            ir::Id actual_target) {
  const auto& operands = instruction.operands;
  if (rule == "add_zero" && instruction.op == ir::Op::Add) {
    if ((is_constant(before, operands[1], 0) && actual_target == operands[0]) ||
        (is_constant(before, operands[0], 0) && actual_target == operands[1]))
      return actual_target;
  } else if (rule == "sub_zero" && instruction.op == ir::Op::Sub) {
    if (is_constant(before, operands[1], 0) && actual_target == operands[0])
      return actual_target;
  } else if (rule == "mul_one" && instruction.op == ir::Op::Mul) {
    if ((is_constant(before, operands[1], 1) && actual_target == operands[0]) ||
        (is_constant(before, operands[0], 1) && actual_target == operands[1]))
      return actual_target;
  } else if (rule == "select_value" &&
             instruction.op == ir::Op::Select) {
    std::set<ir::Id> valid;
    if (operands[1] == operands[2]) valid.insert(operands[1]);
    std::uint32_t condition = 0;
    if (constant_value(before, operands[0], condition))
      valid.insert(condition ? operands[1] : operands[2]);
    if (valid.count(actual_target)) return actual_target;
  }
  throw std::invalid_argument("local copy rewrite side conditions do not hold");
}

std::uint32_t expected_constant(const ir::Function& before,
                                const ir::Instruction& instruction,
                                std::string_view rule,
                                std::uint32_t actual) {
  const auto& operands = instruction.operands;
  bool valid = false;
  std::uint32_t expected = 0;
  if (rule == "sub_self" && instruction.op == ir::Op::Sub) {
    valid = operands[0] == operands[1];
  } else if (rule == "eq_self" && instruction.op == ir::Op::Eq) {
    valid = operands[0] == operands[1];
    expected = 1;
  } else if (rule == "ult_self" && instruction.op == ir::Op::Ult) {
    valid = operands[0] == operands[1];
  } else if (rule == "mul_zero" && instruction.op == ir::Op::Mul) {
    valid = is_constant(before, operands[0], 0) ||
            is_constant(before, operands[1], 0);
  }
  if (!valid || actual != expected)
    throw std::invalid_argument(
        "local constant rewrite side conditions do not hold");
  return expected;
}

SMTProofObligation emit_local_replacement(const ir::Function& before,
                                          const ir::Function& after,
                                          const query::Rewrite& rewrite,
                                          std::size_t ordinal) {
  const auto location = locate(before, rewrite.instruction);
  const auto& original_instruction = instruction_at(before, location);
  const auto after_location = locate(after, rewrite.instruction);
  if (after_location.block != location.block ||
      after_location.index != location.index)
    throw std::invalid_argument(
        "local replacement moved the rewritten instruction");
  const auto& replacement_instruction = instruction_at(after, after_location);

  auto expected = before;
  if (rewrite.rule == "add_zero" || rewrite.rule == "sub_zero" ||
      rewrite.rule == "mul_one" || rewrite.rule == "select_value") {
    if (replacement_instruction.op != ir::Op::Copy ||
        replacement_instruction.operands.size() != 1)
      throw std::invalid_argument(
          "local copy rewrite did not produce the recorded copy");
    const auto target = expected_copy_target(
        before, original_instruction, rewrite.rule,
        replacement_instruction.operands[0]);
    make_copy(instruction_at(expected, location), target);
  } else {
    if (replacement_instruction.op != ir::Op::Const)
      throw std::invalid_argument(
          "local constant rewrite did not produce a constant");
    const auto value = expected_constant(
        before, original_instruction, rewrite.rule,
        replacement_instruction.immediate);
    make_constant(instruction_at(expected, location), value);
  }
  require_exact_result(expected, after, rewrite.rule);

  BooleanCircuit circuit;
  ValueWords words(before, circuit);
  const auto original = words.expression(original_instruction);
  const auto replacement = words.expression(replacement_instruction);
  return finish_value_obligation(
      rewrite, ordinal, original, replacement, circuit,
      "the local algebraic replacement equals the original modular i32 expression");
}

SMTProofObligation emit_local_cse(const ir::Function& before,
                                  const ir::Function& after,
                                  const query::Rewrite& rewrite,
                                  std::size_t ordinal) {
  const auto location = locate(before, rewrite.instruction);
  const auto& original = instruction_at(before, location);
  if (!ir::pure(original.op) || original.op == ir::Op::Phi)
    throw std::invalid_argument("local CSE record names a non-CSE expression");

  const auto after_location = locate(after, rewrite.instruction);
  if (after_location.block != location.block ||
      after_location.index != location.index)
    throw std::invalid_argument("local CSE moved the rewritten instruction");
  const auto& replacement = instruction_at(after, after_location);
  if (replacement.op != ir::Op::Copy || replacement.operands.size() != 1)
    throw std::invalid_argument("local CSE did not produce a single copy");

  const auto source_location = locate(before, replacement.operands[0]);
  if (source_location.block != location.block ||
      source_location.index >= location.index)
    throw std::invalid_argument(
        "local CSE source is not an earlier expression in the same block");
  const auto& source = instruction_at(before, source_location);
  if (!ir::pure(source.op) || source.op == ir::Op::Phi ||
      !same_expression(original, source))
    throw std::invalid_argument(
        "local CSE source and replacement expressions do not match");

  auto expected = before;
  make_copy(instruction_at(expected, location), source.id);
  require_exact_result(expected, after, rewrite.rule);

  BooleanCircuit circuit;
  ValueWords words(before, circuit);
  // The optimizer's value-numbering key, checked above, establishes that the
  // source and eliminated instructions have the same pure expression shape.
  // Encode both evaluations independently while sharing their operand words,
  // so the solver checks congruence instead of receiving a reflexive output.
  const auto original_value = words.expression(original);
  const auto source_value = words.expression(source);
  return finish_value_obligation(
      rewrite, ordinal, original_value, source_value, circuit,
      "the eliminated local expression equals the earlier available expression");
}

SMTProofObligation emit_copy_propagation(const ir::Function& before,
                                         const ir::Function& after,
                                         const query::Rewrite& rewrite,
                                         std::size_t ordinal) {
  const auto location = locate(before, rewrite.instruction);
  const auto& copy = instruction_at(before, location);
  if (copy.op != ir::Op::Copy || copy.operands.size() != 1)
    throw std::invalid_argument(
        "copy-propagation record does not name a copy instruction");
  const auto source = copy.operands[0];
  auto expected = before;
  replace_uses(expected, copy.id, source);
  erase_instruction(expected, location);
  require_exact_result(expected, after, rewrite.rule);

  BooleanCircuit circuit;
  ValueWords words(before, circuit);
  const auto value = words.value(source);
  return finish_value_obligation(
      rewrite, ordinal, value, value, circuit,
      "every propagated copy use has the source i32 value",
      kCongruenceEncoding);
}

SMTProofObligation emit_phi_uniform(const ir::Function& before,
                                    const ir::Function& after,
                                    const query::Rewrite& rewrite,
                                    std::size_t ordinal) {
  const auto location = locate(before, rewrite.instruction);
  const auto& phi = instruction_at(before, location);
  if (phi.op != ir::Op::Phi || phi.operands.empty())
    throw std::invalid_argument(
        "uniform-phi record does not name a nonempty phi");
  const auto source = phi.operands.front();
  if (source == phi.id ||
      !std::all_of(phi.operands.begin(), phi.operands.end(),
                   [&](ir::Id value) { return value == source; }))
    throw std::invalid_argument("uniform-phi side conditions do not hold");

  auto expected = before;
  replace_uses(expected, phi.id, source);
  erase_instruction(expected, location);
  require_exact_result(expected, after, rewrite.rule);

  BooleanCircuit circuit;
  ValueWords words(before, circuit);
  const auto value = words.value(source);
  return finish_value_obligation(
      rewrite, ordinal, value, value, circuit,
      "the phi value equals its identical value on every incoming edge",
      kCongruenceEncoding);
}

SMTProofObligation emit_branch(const ir::Function& before,
                               const ir::Function& after,
                               const query::Rewrite& rewrite,
                               std::size_t ordinal) {
  const auto location = locate(before, rewrite.instruction);
  const auto& branch = instruction_at(before, location);
  if (branch.op != ir::Op::Branch)
    throw std::invalid_argument("fold_branch does not identify a branch");
  std::uint32_t condition = 0;
  const bool direct = constant_value(before, branch.operands.front(), condition);
  if (!direct && branch.targets[0] != branch.targets[1])
    throw std::invalid_argument("fold_branch requires a constant or equal successors");
  const auto target = branch.targets[!condition];

  auto expected = before;
  instruction_at(expected, location) = {branch.id, ir::Op::Jump, 0, {}, {target}};
  std::set<ir::Id> reached{before.entry};
  std::set<std::pair<ir::Id, ir::Id>> edges;
  std::vector<ir::Id> pending{before.entry};
  for (std::size_t i = 0; i < pending.size(); ++i) {
    const auto from = pending[i];
    for (const auto to : expected.blocks.at(from).instructions.back().targets) {
      edges.emplace(from, to);
      if (reached.insert(to).second) pending.push_back(to);
    }
  }
  std::erase_if(expected.blocks, [&](const auto& entry) {
    return !reached.contains(entry.first);
  });
  for (auto& [block, body] : expected.blocks) {
    for (auto& phi : body.instructions) {
      if (phi.op != ir::Op::Phi) break;
      // Traverse the original pairs in order. A predecessor whose block
      // survives may still lose this particular edge (including a backedge).
      const auto& original = phi;
      std::vector<ir::Id> predecessors, values;
      for (std::size_t i = 0; i < original.targets.size(); ++i) {
        if (!edges.contains({original.targets[i], block})) continue;
        predecessors.push_back(original.targets[i]);
        values.push_back(original.operands[i]);
      }
      phi.targets = std::move(predecessors);
      phi.operands = std::move(values);
    }
  }
  require_exact_result(expected, after, rewrite.rule);

  // On any dynamic visit the original chooses the same successor as the
  // jump. The graph reconstruction above checks that every retained edge
  // still passes exactly the old pairings to *all* simultaneous phis.
  BooleanCircuit circuit;
  ValueWords words(before, circuit);
  // Two possible edge labels, or one when the branch names the same successor.
  const auto test = circuit.nonzero(words.value(branch.operands.front()));
  const auto taken = circuit.observe(
      "(or (and " + test + " " +
      (branch.targets[0] == target ? "true" : "false") + ") (and (not " +
      test + ") " + (branch.targets[1] == target ? "true" : "false") + "))");
  circuit.violation("(not " + taken + ")");
  return finish_obligation(rewrite, ordinal, circuit,
      "the original branch always picks the jump successor; CFG and phi repairs are exact",
      "cfg-successor-phi-v1");
}

SMTProofObligation emit_forward_load(const ir::Function& before,
                                     const ir::Function& after,
                                     const query::Rewrite& rewrite,
                                     std::size_t ordinal) {
  const auto location = locate(before, rewrite.instruction);
  const auto& block = before.blocks.at(location.block).instructions;
  const auto& load = block[location.index];
  if (load.op != ir::Op::Load)
    throw std::invalid_argument("forward_load does not identify a load");
  const auto address = load.operands.front();
  const auto same_address = [&](ir::Id a) {
    std::uint32_t x = 0, y = 0;
    return a == address || (constant_value(before, a, x) &&
                            constant_value(before, address, y) && x == y);
  };
  const auto distinct_address = [&](ir::Id a) {
    std::uint32_t x = 0, y = 0;
    return a != address && constant_value(before, a, x) &&
           constant_value(before, address, y) && x != y;
  };
  std::optional<std::size_t> witness;
  for (std::size_t pos = location.index; pos-- > 0;) {
    const auto& i = block[pos];
    if (i.op != ir::Op::Load && i.op != ir::Op::Store) continue;
    if (same_address(i.operands[0])) {
      witness = pos;
      break;
    }
    if (i.op == ir::Op::Store && !distinct_address(i.operands[0]))
      throw std::invalid_argument("forward_load crosses a potentially aliasing store");
  }
  if (!witness)
    throw std::invalid_argument("forward_load lacks an earlier same-block access");
  const auto& access = block[*witness];
  const auto source = access.op == ir::Op::Load ? access.id : access.operands[1];
  auto expected = before;
  make_copy(instruction_at(expected, location), source);
  require_exact_result(expected, after, rewrite.rule);

  // One-cell projection of memory: other cells are immaterial, but every
  // intervening store still has its actual address, bounds check and update
  // on alias. Size is fixed; sizes >= 2^32 allow all i32 addresses.
  BooleanCircuit circuit(false);
  ValueWords words(before, circuit);
  const auto length = circuit.input_word("memory_length_low");
  const auto full = circuit.input_bit("memory_covers_all_i32_addresses");
  const auto in_bounds = [&](const Word& a) {
    return circuit.observe("(or " + full + " " +
                           circuit.less_unsigned(a, length)[0] + ")");
  };
  const auto destination = words.value(address);
  const auto accessed = words.value(access.operands[0]);
  const auto initial = circuit.input_word("memory_at_load_address_before_witness");
  const auto stored = access.op == ir::Op::Store
                          ? words.value(access.operands[1])
                          : circuit.input_word("value_read_by_witness");
  const auto equal = [&](const Word& other) {
    return circuit.equal_word(destination, other)[0];
  };
  const auto aliases_witness = equal(accessed);
  auto prefix = in_bounds(accessed);
  if (access.op == ir::Op::Load) {
    // At a coinciding address the witness read is exactly the initial cell.
    // At other addresses its value is unconstrained in this projection.
    prefix = circuit.observe("(and " + prefix + " (or (not " +
        aliases_witness + ") (not " + BooleanCircuit::differs(initial, stored) + ")))");
  }
  auto cell = access.op == ir::Op::Store
                  ? circuit.choose(aliases_witness, stored, initial) : initial;
  for (std::size_t pos = *witness + 1; pos < location.index; ++pos) {
    const auto& i = block[pos];
    if (i.op != ir::Op::Load && i.op != ir::Op::Store) continue;
    const auto current = words.value(i.operands[0]);
    prefix = circuit.observe("(and " + prefix + ' ' + in_bounds(current) + ")");
    if (i.op == ir::Op::Store)
      cell = circuit.choose(equal(current), words.value(i.operands[1]), cell);
  }
  // If a prior access traps, both executions stop there, before this edit.
  // Otherwise the original load must succeed and read the copied value.
  circuit.violation("(and " + prefix + " (or (not " + in_bounds(destination) +
                    ") " + BooleanCircuit::differs(cell, stored) + "))");
  return finish_obligation(rewrite, ordinal, circuit,
      "after the same successful access prefix the original load is in bounds and reads the forwarded value",
      "i32-bool-memory-cell-v1");
}

SMTProofObligation emit_dead(const ir::Function& before,
                             const ir::Function& after,
                             const query::Rewrite& rewrite,
                             std::size_t ordinal) {
  std::map<ir::Id, const ir::Instruction*> instructions;
  std::set<ir::Id> used, rooted;
  std::vector<ir::Id> pending;
  for (const auto& [id, block] : before.blocks) {
    (void)id;
    for (const auto& i : block.instructions) {
      instructions.emplace(i.id, &i);
      used.insert(i.operands.begin(), i.operands.end());
      // Every terminator and every potentially trapping/effectful operation
      // is a root, even a load whose result is never used.
      if (!ir::pure(i.op)) {
        rooted.insert(i.id);
        pending.push_back(i.id);
      }
    }
  }
  for (std::size_t i = 0; i < pending.size(); ++i)
    for (const auto operand : instructions.at(pending[i])->operands)
      if (rooted.insert(operand).second) pending.push_back(operand);
  std::set<ir::Id> removed;
  if (rewrite.rule == "dead_pure") {
    const auto it = instructions.find(rewrite.instruction);
    if (it == instructions.end() || !ir::pure(it->second->op) ||
        used.contains(rewrite.instruction))
      throw std::invalid_argument("dead_pure requires a pure, unused instruction");
    removed.insert(rewrite.instruction);
  } else {
    for (const auto& [id, i] : instructions) {
      (void)i;
      if (rooted.contains(id)) continue;
      if (!used.contains(id))
        throw std::invalid_argument("dead_pure_cycle requires a dead group after unused definitions");
      removed.insert(id);
    }
    if (removed.empty() || *removed.begin() != rewrite.instruction)
      throw std::invalid_argument("dead_pure_cycle does not identify the exact unrooted group");
  }
  auto expected = before;
  for (auto& [id, block] : expected.blocks) {
    (void)id;
    std::erase_if(block.instructions, [&](const auto& i) { return removed.contains(i.id); });
  }
  require_exact_result(expected, after, rewrite.rule);

  // SMT checks a concrete survivor set is an inductive root/operand cut:
  // no root is removed, and no retained instruction depends on one removed.
  // This handles mutually recursive phis without assuming values for an
  // unrooted cycle. Exact replay above keeps observable CFG and order fixed.
  BooleanCircuit circuit;
  std::map<ir::Id, std::string> keep;
  for (const auto& [id, i] : instructions) {
    (void)i;
    keep[id] = circuit.observe(removed.contains(id) ? "false" : "true");
  }
  for (const auto& [id, i] : instructions) {
    const auto& user = keep.at(id);
    if (!ir::pure(i->op)) circuit.violation("(not " + user + ")");
    for (const auto operand : i->operands)
      circuit.violation("(and " + user + " (not " + keep.at(operand) + "))");
  }
  return finish_obligation(rewrite, ordinal, circuit,
      "every observable root survives and every retained operand remains defined in the unchanged CFG",
      "rooted-ssa-cut-v1");
}

void check_propagation_envelope(const ir::Function& before,
                                const query::Propagation& summary) {
  // Require the advertised predecessor graph to be rooted at entry, and
  // consist exclusively of actual CFG edges. The induction checks below
  // additionally prove that this set covers *all* dynamically taken edges.
  if (!summary.blocks.contains(before.entry))
    throw std::invalid_argument("SCCP summary omits entry");
  std::set<ir::Id> reached{before.entry};
  std::vector<ir::Id> pending{before.entry};
  std::size_t edges = 0;
  for (std::size_t i = 0; i < pending.size(); ++i) {
    const auto block = pending[i];
    const auto& term = before.blocks.at(block).instructions.back();
    std::set<ir::Id> targets(term.targets.begin(), term.targets.end());
    for (const auto to : targets) {
      if (!summary.edges.contains({block, to})) continue;
      ++edges;
      if (reached.insert(to).second) pending.push_back(to);
    }
  }
  if (reached != summary.blocks || edges != summary.edges.size())
    throw std::invalid_argument("SCCP summary contains non-rooted or non-CFG edges");
}

SMTProofObligation emit_conditional_constant(const ir::Function& before,
                                            const ir::Function& after,
                                            const query::Rewrite& rewrite,
                                            std::size_t ordinal,
                                            std::size_t& analysis_budget) {
  using ir::Op;
  using query::ValueKind;
  const auto location = locate(before, rewrite.instruction);
  const auto& original = instruction_at(before, location);
  if (!ir::pure(original.op) || !ir::produces_value(original.op) ||
      original.op == Op::Const)
    throw std::invalid_argument("conditional_constant does not identify a replaceable value");
  const auto summary = [&] {
    try {
      return query::propagate(before, analysis_budget);
    } catch (const datalog::BudgetExceeded&) {
      throw std::length_error("PCC SCCP proof analysis work limit exceeded");
    }
  }();
  analysis_budget -= summary.work;
  check_propagation_envelope(before, summary);
  const auto fact = summary.values.at(original.id);
  if (!summary.blocks.contains(location.block) || fact.kind != ValueKind::Constant)
    throw std::invalid_argument("conditional_constant lacks a reachable constant fact");
  auto expected = before;
  auto& instructions = expected.blocks.at(location.block).instructions;
  make_constant(instructions[location.index], fact.value);
  if (original.op == Op::Phi) {
    auto replacement = instructions[location.index];
    erase_instruction(expected, location);
    const auto first = std::find_if(instructions.begin(), instructions.end(),
                                   [](const auto& i) { return i.op != Op::Phi; });
    instructions.insert(first, std::move(replacement));
  }
  require_exact_result(expected, after, rewrite.rule);

  // Treat propagation as an UNTRUSTED invariant generator: check every
  // constant-producing transfer and every omitted edge, not just the edited
  // definition. Verifier dominance guarantees each operand is available;
  // phis read the preceding state on their named edge, not newly written
  // sibling values. Induction on execution steps thereby handles backedges,
  // including cycles whose purported constants would fail on the first visit.
  BooleanCircuit circuit;
  ValueWords words(before, circuit, &summary);
  for (const auto block : summary.blocks) {
    for (const auto& i : before.blocks.at(block).instructions) {
      if (i.op == Op::Branch || i.op == Op::Jump) {
        const bool first = summary.edges.contains({block, i.targets[0]});
        if (i.op == Op::Jump || i.targets[0] == i.targets[1]) {
          if (!first) throw std::invalid_argument("SCCP summary omits an unconditional edge");
        } else {
          const bool second = summary.edges.contains({block, i.targets[1]});
          if (!first && !second)
            throw std::invalid_argument("SCCP summary omits both branch edges");
          if (!first || !second) {
            const auto test = circuit.nonzero(words.value(i.operands[0]));
            circuit.violation(first ? "(not " + test + ")" : test);
          }
        }
      } else if (ir::produces_value(i.op)) {
        const auto& value = summary.values.at(i.id);
        if (value.kind == ValueKind::Unknown)
          throw std::invalid_argument("SCCP fixed point has an unknown value in an executable block");
        if (value.kind != ValueKind::Constant) continue;
        const auto claimed = BooleanCircuit::constant(value.value);
        if (i.op == Op::Phi) {
          bool incoming = false;
          for (std::size_t slot = 0; slot < i.targets.size(); ++slot) {
            if (!summary.edges.contains({i.targets[slot], block})) continue;
            incoming = true;
            circuit.violation(BooleanCircuit::differs(words.value(i.operands[slot]), claimed));
          }
          if (!incoming) throw std::invalid_argument("SCCP phi has no executable predecessor");
        } else {
          // An arbitrary input cannot be assumed constant, even a load from
          // a known address. No memory invariants participate in SCCP.
          const auto actual = i.op == Op::Load
              ? circuit.input_word("load_result_" + std::to_string(i.id))
              : words.expression(i);
          circuit.violation(BooleanCircuit::differs(actual, claimed));
        }
      }
    }
  }
  return finish_obligation(rewrite, ordinal, circuit,
      "the reachable-edge and constant invariant is inductive under every executable transfer",
      "i32-bool-sccp-invariant-v1");
}

bool same_rewrite_record(const query::Rewrite& left,
                         const query::Rewrite& right) {
  return left.revision == right.revision &&
         left.instruction == right.instruction && left.rule == right.rule &&
         left.dependencies == right.dependencies;
}

SMTProofObligation emit_verified_obligation(const ir::Function& before,
                                            const ir::Function& after,
                                            const query::Rewrite& rewrite,
                                            std::size_t ordinal,
                                            std::size_t& analysis_budget) {
  if (!is_known_rule(rewrite.rule))
    throw std::invalid_argument("unknown optimizer rewrite rule: " +
                                rewrite.rule);
  if (is_fold_rule(rewrite.rule))
    return emit_fold(before, after, rewrite, ordinal);
  if (is_local_replacement_rule(rewrite.rule))
    return emit_local_replacement(before, after, rewrite, ordinal);
  if (rewrite.rule == "local_cse")
    return emit_local_cse(before, after, rewrite, ordinal);
  if (rewrite.rule == "copy_propagation")
    return emit_copy_propagation(before, after, rewrite, ordinal);
  if (rewrite.rule == "phi_uniform")
    return emit_phi_uniform(before, after, rewrite, ordinal);
  if (rewrite.rule == "fold_branch")
    return emit_branch(before, after, rewrite, ordinal);
  if (rewrite.rule == "forward_load")
    return emit_forward_load(before, after, rewrite, ordinal);
  if (rewrite.rule == "dead_pure" || rewrite.rule == "dead_pure_cycle")
    return emit_dead(before, after, rewrite, ordinal);
  if (rewrite.rule == "conditional_constant")
    return emit_conditional_constant(before, after, rewrite, ordinal, analysis_budget);
  throw std::invalid_argument("rewrite has no sound SMT proof obligation");
}

template<class CheckRecord>
SMTProofArtifact replay_proof(const ccdb::Snapshot& base,
                              const ir::Function& endpoint,
                              const query::Options& options,
                              std::size_t count, CheckRecord check_record) {
  auto current = ir::load(base);
  ir::verify(current);
  SMTProofArtifact artifact;
  // A baseline/empty accepted prefix is meaningful only for identical IR.
  if (count == 0) {
    require_exact_result(current, endpoint, "empty prefix");
    return artifact;
  }
  std::size_t analysis_budget = options.analysis_work;
  std::size_t remaining_bytes = kMaximumProofBytes;
  const auto replay = query::optimize(base, options,
      [&](const query::Rewrite& rewrite, const ir::Function& next) {
        const auto index = artifact.obligations.size();
        if (index >= count || rewrite.revision != index + 1)
          throw std::invalid_argument("unexpected rewrite index or revision during PCC replay");
        check_record(index, rewrite);
        auto obligation = emit_verified_obligation(current, next, rewrite, index,
                                                   analysis_budget);
        if (obligation.smtlib2.size() > remaining_bytes)
          throw std::length_error("PCC total SMT obligation byte limit exceeded");
        remaining_bytes -= obligation.smtlib2.size();
        artifact.obligations.push_back(std::move(obligation));
        current = next;
      });
  if (artifact.obligations.size() != count || replay.rewrites().size() != count)
    throw std::invalid_argument("PCC could not reconstruct every rewrite within the recorded limits");
  require_exact_result(current, endpoint, "PCC replay endpoint");
  require_exact_result(ir::load(replay.result()), endpoint, "optimizer replay endpoint");
  return artifact;
}

#if defined(NOVOSTONE_PCC_USE_SATIE) && NOVOSTONE_PCC_USE_SATIE
bool unsupported_satie_diagnostic(std::string_view message) {
  return message.find("unsupported") != std::string_view::npos ||
         message.find("only Bool and Int sorts") != std::string_view::npos;
}

SMTSyntaxResult syntax_from_satie_error(const satie::ParseError& error) {
  if (unsupported_satie_diagnostic(error.diagnostic.message))
    return {SMTSyntaxStatus::Unknown,
            "Satie SMT-LIB2 frontend does not support this syntax or theory: " +
                error.diagnostic.message};
  return {SMTSyntaxStatus::Invalid, error.what()};
}
#endif

} // namespace

SMTFrontend parse_smt_frontend(std::string_view frontend) {
  if (frontend == "smtlib2") return SMTFrontend::SMTLIB2;
  throw std::invalid_argument("unsupported PCC SMT frontend: " +
                              std::string(frontend));
}

std::string_view name(SMTFrontend frontend) {
  if (frontend == SMTFrontend::SMTLIB2) return "smtlib2";
  throw std::invalid_argument("invalid PCC SMT frontend");
}

std::string_view name(ObligationStatus status) {
  switch (status) {
    case ObligationStatus::Unchecked:
      return "unchecked";
    case ObligationStatus::UnsatProved:
      return "unsat-proved";
    case ObligationStatus::SatCounterexample:
      return "sat-counterexample";
    case ObligationStatus::Unknown:
      return "unknown";
  }
  throw std::invalid_argument("invalid PCC obligation status");
}

std::string_view name(SMTSyntaxStatus status) {
  switch (status) {
    case SMTSyntaxStatus::Valid:
      return "valid";
    case SMTSyntaxStatus::Invalid:
      return "invalid";
    case SMTSyntaxStatus::Unknown:
      return "unknown";
  }
  throw std::invalid_argument("invalid PCC SMT syntax status");
}

std::string smt_identifier(std::string_view text) {
  if (text.size() >
      (std::numeric_limits<std::size_t>::max() - 2) / 2)
    throw std::length_error("SMT identifier input is too large");
  static constexpr char digits[] = "0123456789abcdef";
  std::string result = "n_";
  result.reserve(2 + text.size() * 2);
  for (unsigned char byte : text) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 0x0f]);
  }
  return result;
}

bool SMTProofArtifact::all_proved() const {
  return std::all_of(
      obligations.begin(), obligations.end(),
      [](const auto& obligation) {
        return obligation.status == ObligationStatus::UnsatProved;
      });
}

SMTProofObligation emit_smt_obligation(const ir::Function& before,
                                       const ir::Function& after,
                                       const query::Rewrite& rewrite,
                                       std::size_t ordinal) {
  ir::verify(before);
  ir::verify(after);
  std::size_t budget = WorkLimits{}.max_analysis_work;
  return emit_verified_obligation(before, after, rewrite, ordinal, budget);
}

SMTProofArtifact emit_smt_proof(const query::Candidate& candidate) {
  auto options = candidate.options();
  const auto count = candidate.rewrites().size();
  if (count > options.rewrites)
    throw std::invalid_argument("candidate exceeds its recorded rewrite limit");
  options.rewrites = count;
  return replay_proof(candidate.base(), ir::load(candidate.result()), options,
      count, [&](std::size_t index, const query::Rewrite& rewrite) {
        if (!same_rewrite_record(candidate.rewrites()[index], rewrite))
          throw std::invalid_argument(
              "candidate rewrite record cannot be deterministically reconstructed");
      });
}

SMTProofArtifact emit_smt_proof(const ir::Function& before,
                                 const ir::Function& after,
                                 const query::Candidate& candidate) {
  ir::verify(before);
  ir::verify(after);
  if (!same_function(before, ir::load(candidate.base())) ||
      !same_function(after, ir::load(candidate.result())))
    throw std::invalid_argument(
        "supplied proof endpoints do not match the Candidate");
  return emit_smt_proof(candidate);
}

SMTProofArtifact emit_smt_proof(const Artifact& artifact) {
  ir::verify(artifact.source());
  ir::verify(artifact.transformed());

  ccdb::Database database(ir::program(),
                          [](const auto& snapshot) { ir::verify(snapshot); });
  const auto imported = ir::publish(database, database.snapshot(),
                                    artifact.source(), "PCC proof replay");

  query::Options options;
  const auto count = artifact.transformations().size();
  if (count > artifact.constraints().limits.max_rewrites)
    throw std::invalid_argument("PCC artifact exceeds its rewrite limit");
  options.rewrites = count;
  options.query_work = artifact.constraints().limits.max_query_work;
  options.query_facts = artifact.constraints().limits.max_query_facts;
  options.analysis_work = artifact.constraints().limits.max_analysis_work;
  options.conditional_constants = artifact.constraints().allows(
      TransformKind::ConditionalConstantPropagation);
  return replay_proof(imported, artifact.transformed(), options, count,
      [&](std::size_t index, const query::Rewrite& replayed) {
        const auto& recorded = artifact.transformations()[index];
        if (recorded.sequence != index ||
            recorded.source_revision != replayed.revision ||
            recorded.instruction != replayed.instruction ||
            recorded.rule != replayed.rule ||
            recorded.kind != rewrite_kind(replayed.rule) ||
            !artifact.constraints().allows(recorded.kind) ||
            !std::ranges::equal(recorded.dependencies, replayed.dependencies))
          throw std::invalid_argument(
              "PCC artifact transformation record does not match replay");
      });
}

bool satie_smtlib2_available() noexcept {
#if defined(NOVOSTONE_PCC_USE_SATIE) && NOVOSTONE_PCC_USE_SATIE
  return true;
#else
  return false;
#endif
}

SMTSyntaxResult validate_smtlib2_syntax(std::string_view script) {
#if defined(NOVOSTONE_PCC_USE_SATIE) && NOVOSTONE_PCC_USE_SATIE
  try {
    satie::frontend::SMTLIB2Script parsed;
    parsed.load_text(std::string(script));
    return {SMTSyntaxStatus::Valid, {}};
  } catch (const satie::ParseError& error) {
    return syntax_from_satie_error(error);
  } catch (const std::exception& error) {
    return {SMTSyntaxStatus::Unknown,
            std::string("Satie SMT-LIB2 frontend failed: ") + error.what()};
  }
#else
  (void)script;
  return {SMTSyntaxStatus::Unknown,
          "Satie SMT-LIB2 conduit was not compiled in "
          "(define NOVOSTONE_PCC_USE_SATIE)"};
#endif
}

SMTConduitResult run_smtlib2(std::string_view script) {
#if defined(NOVOSTONE_PCC_USE_SATIE) && NOVOSTONE_PCC_USE_SATIE
  try {
    satie::frontend::SMTLIB2Script parsed;
    parsed.load_text(std::string(script));
    if (!parsed.has_check_sat())
      return {SMTSyntaxStatus::Valid, ObligationStatus::Unknown,
              "SMT-LIB2 script has no (check-sat)", {}};
    const auto solved = parsed.last_result();
    SMTConduitResult result;
    result.syntax = SMTSyntaxStatus::Valid;
    result.counterexample = solved.model.bools;
    switch (solved.status) {
      case satie::SolveStatus::UNSAT:
        result.status = ObligationStatus::UnsatProved;
        result.counterexample.clear();
        result.diagnostic = "Satie proved the negated claim unsatisfiable";
        break;
      case satie::SolveStatus::SAT:
        result.status = ObligationStatus::SatCounterexample;
        result.diagnostic =
            "Satie found a satisfying assignment for the negated claim";
        break;
      case satie::SolveStatus::UNKNOWN:
        result.status = ObligationStatus::Unknown;
        result.counterexample.clear();
        result.diagnostic =
            "Satie returned unknown for the requested SMT-LIB2 fragment";
        break;
    }
    return result;
  } catch (const satie::ParseError& error) {
    const auto syntax = syntax_from_satie_error(error);
    return {syntax.status, ObligationStatus::Unknown, syntax.diagnostic, {}};
  } catch (const std::exception& error) {
    return {SMTSyntaxStatus::Unknown, ObligationStatus::Unknown,
            std::string("Satie SMT-LIB2 conduit failed: ") + error.what(),
            {}};
  }
#else
  (void)script;
  return {SMTSyntaxStatus::Unknown, ObligationStatus::Unknown,
          "Satie SMT-LIB2 conduit was not compiled in "
          "(define NOVOSTONE_PCC_USE_SATIE)",
          {}};
#endif
}

SMTConduitResult run_smtlib2(const SMTProofObligation& obligation) {
  return run_smtlib2(obligation.smtlib2);
}

SMTConduitResult discharge_smt(SMTProofObligation& obligation,
                               SMTFrontend frontend) {
  if (frontend != SMTFrontend::SMTLIB2)
    throw std::invalid_argument("unsupported PCC SMT frontend");
  auto result = run_smtlib2(obligation);
  obligation.status = result.status;
  obligation.diagnostic = result.diagnostic;
  obligation.counterexample = result.counterexample;
  return result;
}

void discharge_smt(SMTProofArtifact& artifact, SMTFrontend frontend) {
  if (frontend != artifact.frontend)
    throw std::invalid_argument(
        "proof artifact frontend does not match discharge frontend");
  for (auto& obligation : artifact.obligations)
    (void)discharge_smt(obligation, frontend);
}

} // namespace novostone::pcc
