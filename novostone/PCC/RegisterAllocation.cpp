#include "novostone/PCC/RegisterAllocation.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace novostone::pcc {
namespace {

constexpr const char* native_limitation =
    "abstract SSA allocation only: this evidence does not check native calls, "
    "instruction constraints, clobbers, ABI boundary moves, spills, "
    "or prologue/epilogue";

InterferenceEdge canonical_edge(ir::Id first, ir::Id second) {
  return first < second ? InterferenceEdge{first, second}
                        : InterferenceEdge{second, first};
}

std::set<ir::Id> value_ids(const ir::Function& function) {
  std::set<ir::Id> result;
  for (const auto& [block_id, block] : function.blocks) {
    (void)block_id;
    for (const auto& instruction : block.instructions) {
      if (ir::produces_value(instruction.op)) result.insert(instruction.id);
    }
  }
  return result;
}

void add_edge(InterferenceGraph& graph, ir::Id first, ir::Id second) {
  if (first == second) return;
  const auto edge = canonical_edge(first, second);
  if (!graph.edges.insert(edge).second) return;
  graph.neighbors[first].insert(second);
  graph.neighbors[second].insert(first);
}

void add_clique(InterferenceGraph& graph, const std::set<ir::Id>& live) {
  for (auto first = live.begin(); first != live.end(); ++first) {
    for (auto second = std::next(first); second != live.end(); ++second) {
      add_edge(graph, *first, *second);
    }
  }
}

void check_register(PhysicalRegister physical_register,
                    const CallingConvention& convention,
                    const char* description) {
  if (physical_register >= convention.register_count) {
    throw std::invalid_argument(std::string(description) +
                                " is outside the physical register range");
  }
}

void validate_options(const AllocationOptions& options) {
  if (options.search_node_limit == 0) {
    throw std::invalid_argument("register allocation search limit must be positive");
  }
  if (options.smt_boolean_variable_limit == 0 ||
      options.smt_constraint_limit == 0) {
    throw std::invalid_argument("register allocation SMT limits must be positive");
  }
}

std::string value_text(ir::Id value) {
  return "%" + std::to_string(value);
}

std::string register_text(PhysicalRegister physical_register) {
  return "r" + std::to_string(physical_register);
}

struct PreparedAllocation {
  InterferenceGraph graph;
  std::map<ir::Id, std::set<PhysicalRegister>> domains;
  std::map<ir::Id, PhysicalRegister> required;
  std::vector<std::pair<ir::Id, PhysicalRegister>> parameters;
  std::vector<std::pair<ir::Id, PhysicalRegister>> returns;
  std::vector<std::string> conflicts;
};

void require_register(PreparedAllocation& prepared, ir::Id value,
                      PhysicalRegister physical_register,
                      const std::string& reason) {
  const auto [position, inserted] =
      prepared.required.emplace(value, physical_register);
  if (!inserted && position->second != physical_register) {
    prepared.conflicts.push_back(
        value_text(value) + " is required in both " +
        register_text(position->second) + " and " +
        register_text(physical_register) + " (" + reason + ")");
  }
}

PreparedAllocation prepare(const ir::Function& function,
                           const CallingConvention& convention,
                           const AllocationConstraints& constraints) {
  ir::verify(function);
  validate_calling_convention(convention);
  validate_allocation_constraints(function, convention, constraints);

  PreparedAllocation prepared;
  prepared.graph = build_interference_graph(function);
  for (const auto& edge : constraints.additional_interferences) {
    add_edge(prepared.graph, edge.first, edge.second);
  }

  std::set<PhysicalRegister> allocatable = convention.caller_saved;
  allocatable.insert(convention.callee_saved.begin(),
                     convention.callee_saved.end());

  for (const auto value : prepared.graph.values) {
    auto domain = allocatable;
    if (const auto allowed = constraints.allowed_registers.find(value);
        allowed != constraints.allowed_registers.end()) {
      std::set<PhysicalRegister> intersection;
      std::set_intersection(
          domain.begin(), domain.end(), allowed->second.begin(),
          allowed->second.end(),
          std::inserter(intersection, intersection.end()));
      domain = std::move(intersection);
    }
    if (const auto forbidden = constraints.forbidden_registers.find(value);
        forbidden != constraints.forbidden_registers.end()) {
      for (const auto physical_register : forbidden->second) {
        domain.erase(physical_register);
      }
    }
    prepared.domains.emplace(value, std::move(domain));
  }

  if (static_cast<std::uint64_t>(function.arity) >
      static_cast<std::uint64_t>(convention.argument_registers.size())) {
    prepared.conflicts.push_back(
        "function arity exceeds the convention's register argument capacity; "
        "stack arguments are not modeled");
  }

  std::map<std::uint32_t, ir::Id> parameter_definitions;
  for (const auto& [block_id, block] : function.blocks) {
    (void)block_id;
    for (const auto& instruction : block.instructions) {
      if (instruction.op == ir::Op::Param &&
          instruction.immediate < convention.argument_registers.size()) {
        const auto [previous, inserted] =
            parameter_definitions.emplace(instruction.immediate,
                                          instruction.id);
        if (!inserted) {
          prepared.conflicts.push_back(
              "argument " + std::to_string(instruction.immediate) +
              " has multiple SSA Param definitions " +
              value_text(previous->second) + " and " +
              value_text(instruction.id) +
              "; duplicate parameter materialization is unsupported");
        }
        const auto physical_register =
            convention.argument_registers[instruction.immediate];
        prepared.parameters.emplace_back(instruction.id, physical_register);
        require_register(prepared, instruction.id, physical_register,
                         "calling-convention parameter");
      }
      if (instruction.op == ir::Op::Return) {
        const auto value = instruction.operands.at(0);
        prepared.returns.emplace_back(value, convention.return_register);
        require_register(prepared, value, convention.return_register,
                         "calling-convention return");
      }
    }
  }
  for (const auto& [value, physical_register] :
       constraints.fixed_registers) {
    require_register(prepared, value, physical_register,
                     "explicit fixed-register constraint");
  }

  for (const auto& [value, physical_register] : prepared.required) {
    if (!prepared.domains.at(value).count(physical_register)) {
      prepared.conflicts.push_back(
          value_text(value) + " cannot use required " +
          register_text(physical_register) +
          " under its allowed/forbidden register constraints");
    }
  }
  for (const auto& [first, second] : prepared.graph.edges) {
    const auto first_required = prepared.required.find(first);
    const auto second_required = prepared.required.find(second);
    if (first_required != prepared.required.end() &&
        second_required != prepared.required.end() &&
        first_required->second == second_required->second) {
      prepared.conflicts.push_back(
          "interfering values " + value_text(first) + " and " +
          value_text(second) + " are both required in " +
          register_text(first_required->second));
    }
  }
  for (const auto value : prepared.graph.values) {
    if (prepared.domains.at(value).empty()) {
      prepared.conflicts.push_back(value_text(value) +
                                   " has no allocatable physical register");
    }
  }
  return prepared;
}

std::vector<PhysicalRegister> ordered_candidates(
    ir::Id value, const PreparedAllocation& prepared,
    const CallingConvention& convention,
    const RegisterAssignment& assignment) {
  if (const auto required = prepared.required.find(value);
      required != prepared.required.end()) {
    for (const auto neighbor : prepared.graph.neighbors.at(value)) {
      if (const auto allocated = assignment.find(neighbor);
          allocated != assignment.end() &&
          allocated->second == required->second) {
        return {};
      }
    }
    return {required->second};
  }

  std::set<PhysicalRegister> unavailable;
  for (const auto neighbor : prepared.graph.neighbors.at(value)) {
    if (const auto allocated = assignment.find(neighbor);
        allocated != assignment.end()) {
      unavailable.insert(allocated->second);
    }
  }

  std::vector<PhysicalRegister> result;
  const auto append = [&](const std::set<PhysicalRegister>& registers) {
    for (const auto physical_register : registers) {
      if (prepared.domains.at(value).count(physical_register) &&
          !unavailable.count(physical_register)) {
        result.push_back(physical_register);
      }
    }
  };
  append(convention.caller_saved);
  append(convention.callee_saved);
  return result;
}

ir::Id select_value(const PreparedAllocation& prepared,
                    const CallingConvention& convention,
                    const RegisterAssignment& assignment) {
  ir::Id selected = 0;
  std::size_t selected_remaining = std::numeric_limits<std::size_t>::max();
  std::size_t selected_saturation = 0;
  std::size_t selected_degree = 0;

  for (const auto value : prepared.graph.values) {
    if (assignment.count(value)) continue;
    const auto remaining =
        ordered_candidates(value, prepared, convention, assignment).size();
    std::set<PhysicalRegister> saturation;
    for (const auto neighbor : prepared.graph.neighbors.at(value)) {
      if (const auto allocated = assignment.find(neighbor);
          allocated != assignment.end()) {
        saturation.insert(allocated->second);
      }
    }
    const auto degree = prepared.graph.neighbors.at(value).size();
    const auto better =
        selected == 0 || remaining < selected_remaining ||
        (remaining == selected_remaining &&
         saturation.size() > selected_saturation) ||
        (remaining == selected_remaining &&
         saturation.size() == selected_saturation &&
         degree > selected_degree) ||
        (remaining == selected_remaining &&
         saturation.size() == selected_saturation &&
         degree == selected_degree && value < selected);
    if (better) {
      selected = value;
      selected_remaining = remaining;
      selected_saturation = saturation.size();
      selected_degree = degree;
    }
  }
  return selected;
}

enum class SearchResult {
  Found,
  Impossible,
  Exhausted
};

SearchResult color(const PreparedAllocation& prepared,
                   const CallingConvention& convention,
                   const AllocationOptions& options,
                   RegisterAssignment& assignment) {
  for (const auto& [value, physical_register] : prepared.required) {
    assignment.emplace(value, physical_register);
  }
  if (assignment.size() == prepared.graph.values.size()) {
    return SearchResult::Found;
  }

  struct Frame {
    ir::Id value = 0;
    std::vector<PhysicalRegister> candidates;
    std::size_t next = 0;
  };
  std::vector<Frame> stack;
  std::size_t search_nodes = 0;

  for (;;) {
    if (assignment.size() == prepared.graph.values.size()) {
      return SearchResult::Found;
    }

    const auto value =
        select_value(prepared, convention, assignment);
    auto candidates =
        ordered_candidates(value, prepared, convention, assignment);
    if (!candidates.empty()) {
      if (search_nodes == options.search_node_limit) {
        return SearchResult::Exhausted;
      }
      ++search_nodes;
      assignment[value] = candidates.front();
      stack.push_back({value, std::move(candidates), 0});
      continue;
    }

    bool advanced = false;
    while (!stack.empty()) {
      auto& frame = stack.back();
      assignment.erase(frame.value);
      ++frame.next;
      if (frame.next == frame.candidates.size()) {
        stack.pop_back();
        continue;
      }
      if (search_nodes == options.search_node_limit) {
        return SearchResult::Exhausted;
      }
      ++search_nodes;
      assignment[frame.value] = frame.candidates[frame.next];
      advanced = true;
      break;
    }
    if (!advanced) return SearchResult::Impossible;
  }
}

std::string bool_and(const std::vector<std::string>& terms) {
  if (terms.empty()) return "true";
  if (terms.size() == 1) return terms.front();
  std::string result = "(and";
  for (const auto& term : terms) result += " " + term;
  result += ")";
  return result;
}

std::string bool_or(const std::vector<std::string>& terms) {
  if (terms.empty()) return "false";
  if (terms.size() == 1) return terms.front();
  std::string result = "(or";
  for (const auto& term : terms) result += " " + term;
  result += ")";
  return result;
}

std::string allocation_name(ir::Id value,
                            PhysicalRegister physical_register) {
  return "alloc_v" + std::to_string(value) + "_r" +
         std::to_string(physical_register);
}

std::string allocation_literal(ir::Id value,
                               PhysicalRegister physical_register,
                               bool set) {
  const auto name = allocation_name(value, physical_register);
  return set ? name : "(not " + name + ")";
}

std::string register_equals(ir::Id value,
                            PhysicalRegister physical_register) {
  return allocation_name(value, physical_register);
}

std::string register_in_range(ir::Id value, std::uint32_t count) {
  std::vector<std::string> alternatives;
  alternatives.reserve(count);
  for (std::uint64_t physical_register = 0;
       physical_register < count; ++physical_register) {
    alternatives.push_back(allocation_name(
        value, static_cast<PhysicalRegister>(physical_register)));
  }
  return bool_or(alternatives);
}

std::string registers_differ(ir::Id first, ir::Id second,
                             std::uint32_t count) {
  std::vector<std::string> conditions;
  conditions.reserve(count);
  for (std::uint64_t physical_register = 0;
       physical_register < count; ++physical_register) {
    const auto number =
        static_cast<PhysicalRegister>(physical_register);
    conditions.push_back(
        "(not (and " + allocation_name(first, number) + " " +
        allocation_name(second, number) + "))");
  }
  return bool_and(conditions);
}

bool add_within_limit(std::size_t& total, std::size_t amount,
                      std::size_t limit) {
  if (amount > limit - std::min(total, limit)) return false;
  total += amount;
  return total <= limit;
}

std::vector<CalleeSavedPreservation> preservation_obligations(
    const CallingConvention& convention,
    const RegisterAssignment& assignment) {
  std::map<PhysicalRegister, std::set<ir::Id>> uses;
  for (const auto& [value, physical_register] : assignment) {
    if (convention.callee_saved.count(physical_register)) {
      uses[physical_register].insert(value);
    }
  }
  std::vector<CalleeSavedPreservation> result;
  for (auto& [physical_register, values] : uses) {
    result.push_back(
        {physical_register, std::move(values),
         "callee_saved_r" + std::to_string(physical_register) +
             "_preserved"});
  }
  return result;
}

std::string limited_smt(const char* predicate,
                        SolverExpectation expectation,
                        const std::string& reason) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "; Novostone PCC register-allocation obligation v1\n"
      << "; " << reason << "\n"
      << "(set-logic QF_UF)\n"
      << "(set-info :status "
      << (expectation == SolverExpectation::Unsat
              ? "unsat"
              : expectation == SolverExpectation::Sat ? "sat" : "unknown")
      << ")\n"
      << "(declare-const " << predicate << " Bool)\n"
      << "(assert (not " << predicate << "))\n"
      << "(check-sat)\n";
  return out.str();
}

std::string emit_smt(
    const PreparedAllocation& prepared,
    const CallingConvention& convention,
    const AllocationConstraints& constraints,
    const RegisterAssignment& assignment,
    const std::vector<CalleeSavedPreservation>& preservation,
    bool mapping_well_formed, SolverExpectation expectation,
    const AllocationOptions& options, bool& within_budget) {
  const auto values = prepared.graph.values.size();
  const auto registers =
      static_cast<std::size_t>(convention.register_count);
  if (values != 0 &&
      registers > options.smt_boolean_variable_limit / values) {
    within_budget = false;
    return limited_smt(
        "certificate_within_resource_limits", SolverExpectation::Unknown,
        "certificate omitted because its Boolean variable limit was exceeded");
  }
  const auto variable_count = values * registers + preservation.size();
  if (variable_count > options.smt_boolean_variable_limit) {
    within_budget = false;
    return limited_smt(
        "certificate_within_resource_limits", SolverExpectation::Unknown,
        "certificate omitted because its Boolean variable limit was exceeded");
  }

  std::size_t constraint_count = 0;
  bool count_ok = true;
  const auto add_product = [&](std::size_t first, std::size_t second) {
    if (!count_ok) return;
    if (first != 0 &&
        second > options.smt_constraint_limit / first) {
      count_ok = false;
      return;
    }
    count_ok = add_within_limit(
        constraint_count, first * second,
        options.smt_constraint_limit);
  };
  // Concrete one-hot assignment facts and in-range disjunctions.
  add_product(values, registers);
  add_product(values, registers);
  // Every interference edge compares every physical-register selector.
  add_product(prepared.graph.edges.size(), registers);
  add_product(values, convention.reserved.size());
  for (const auto& [value, allowed] : constraints.allowed_registers) {
    (void)value;
    count_ok = count_ok &&
               add_within_limit(constraint_count, allowed.size(),
                                options.smt_constraint_limit);
  }
  for (const auto& [value, forbidden] :
       constraints.forbidden_registers) {
    (void)value;
    count_ok = count_ok &&
               add_within_limit(constraint_count, forbidden.size(),
                                options.smt_constraint_limit);
  }
  for (const auto& obligation : preservation) {
    count_ok = count_ok &&
               add_within_limit(constraint_count, obligation.values.size(),
                                options.smt_constraint_limit);
  }
  for (const auto amount :
       {constraints.fixed_registers.size(), prepared.parameters.size(),
        prepared.returns.size(), preservation.size(), std::size_t{1}}) {
    count_ok = count_ok &&
               add_within_limit(constraint_count, amount,
                                options.smt_constraint_limit);
  }
  if (!count_ok) {
    within_budget = false;
    return limited_smt(
        "certificate_within_resource_limits", SolverExpectation::Unknown,
        "certificate omitted because its constraint limit was exceeded");
  }

  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << "; Novostone PCC register-allocation obligation v1\n"
      << "; Assignment facts are assumptions.  The final assertion negates\n"
      << "; range, liveness interference, ABI boundary, reservation, and\n"
      << "; callee-preservation conditions.  UNSAT proves only this model.\n"
      << "; Native calling-convention correctness remains outside this IR.\n"
      << "(set-logic QF_UF)\n"
      << "(set-info :status "
      << (expectation == SolverExpectation::Unsat
              ? "unsat"
              : expectation == SolverExpectation::Sat ? "sat" : "unknown")
      << ")\n";

  for (const auto value : prepared.graph.values) {
    for (std::uint64_t physical_register = 0;
         physical_register < convention.register_count;
         ++physical_register) {
      out << "(declare-const "
          << allocation_name(
                 value, static_cast<PhysicalRegister>(physical_register))
          << " Bool)\n";
    }
  }
  for (const auto& obligation : preservation) {
    out << "(declare-const " << obligation.predicate << " Bool)\n";
  }

  out << "; concrete assignment\n";
  for (const auto value : prepared.graph.values) {
    const auto allocated = assignment.find(value);
    for (std::uint64_t physical_register = 0;
         physical_register < convention.register_count;
         ++physical_register) {
      const auto number =
          static_cast<PhysicalRegister>(physical_register);
      out << "(assert "
          << allocation_literal(
                 value, number,
                 allocated != assignment.end() &&
                     allocated->second == number)
          << ")\n";
    }
  }

  std::vector<std::string> conditions;
  conditions.reserve(constraint_count);
  if (!mapping_well_formed) conditions.emplace_back("false");

  out << "; register range\n";
  for (const auto value : prepared.graph.values) {
    conditions.push_back(
        register_in_range(value, convention.register_count));
  }

  out << "; liveness, ABI-entry, and additional interference\n";
  for (const auto& [first, second] : prepared.graph.edges) {
    conditions.push_back(
        registers_differ(first, second, convention.register_count));
  }

  out << "; parameter locations\n";
  for (const auto& [value, physical_register] : prepared.parameters) {
    conditions.push_back(
        register_equals(value, physical_register));
  }
  out << "; return locations\n";
  for (const auto& [value, physical_register] : prepared.returns) {
    conditions.push_back(
        register_equals(value, physical_register));
  }

  out << "; explicit allocation constraints\n";
  for (const auto& [value, physical_register] :
       constraints.fixed_registers) {
    conditions.push_back(
        register_equals(value, physical_register));
  }
  for (const auto& [value, allowed] : constraints.allowed_registers) {
    std::vector<std::string> alternatives;
    alternatives.reserve(allowed.size());
    for (const auto physical_register : allowed) {
      alternatives.push_back(
          register_equals(value, physical_register));
    }
    conditions.push_back(bool_or(alternatives));
  }
  for (const auto& [value, forbidden] :
       constraints.forbidden_registers) {
    for (const auto physical_register : forbidden) {
      conditions.push_back(
          "(not " + register_equals(value, physical_register) + ")");
    }
  }

  out << "; reserved registers are unavailable\n";
  for (const auto value : prepared.graph.values) {
    for (const auto physical_register : convention.reserved) {
      conditions.push_back(
          "(not " + register_equals(value, physical_register) + ")");
    }
  }

  out << "; unresolved callee-saved preservation predicates\n";
  for (const auto& obligation : preservation) {
    std::vector<std::string> uses;
    uses.reserve(obligation.values.size());
    for (const auto value : obligation.values) {
      uses.push_back(register_equals(
          value, obligation.physical_register));
    }
    conditions.push_back("(=> " + bool_or(uses) + " " +
                         obligation.predicate + ")");
  }

  out << "(assert (not\n  " << bool_and(conditions) << "))\n"
      << "(check-sat)\n";
  within_budget = true;
  return out.str();
}

void append_mapping_diagnostics(
    const PreparedAllocation& prepared,
    const CallingConvention& convention,
    const AllocationConstraints& constraints,
    const RegisterAssignment& assignment,
    std::vector<std::string>& diagnostics) {
  for (const auto value : prepared.graph.values) {
    if (!assignment.count(value)) {
      diagnostics.push_back("allocation is missing " + value_text(value));
    }
  }
  for (const auto& [value, physical_register] : assignment) {
    if (!prepared.graph.values.count(value)) {
      diagnostics.push_back("allocation contains unknown or non-value SSA ID " +
                            value_text(value));
      continue;
    }
    if (physical_register >= convention.register_count) {
      diagnostics.push_back(
          value_text(value) + " is assigned out-of-range " +
          register_text(physical_register));
      continue;
    }
    if (convention.reserved.count(physical_register)) {
      diagnostics.push_back(value_text(value) + " uses reserved " +
                            register_text(physical_register));
    }
    if (!prepared.domains.at(value).count(physical_register)) {
      diagnostics.push_back(
          value_text(value) + " is outside its allocation domain in " +
          register_text(physical_register));
    }
  }
  for (const auto& [first, second] : prepared.graph.edges) {
    const auto left = assignment.find(first);
    const auto right = assignment.find(second);
    if (left != assignment.end() && right != assignment.end() &&
        left->second == right->second) {
      diagnostics.push_back(
          "interfering values " + value_text(first) + " and " +
          value_text(second) + " share " + register_text(left->second));
    }
  }
  for (const auto& [value, physical_register] : prepared.required) {
    if (const auto allocated = assignment.find(value);
        allocated != assignment.end() &&
        allocated->second != physical_register) {
      diagnostics.push_back(
          value_text(value) + " must use " +
          register_text(physical_register) + " but uses " +
          register_text(allocated->second));
    }
  }

  // Explicitly retain these checks here even though domain validation above
  // catches them, so diagnostics remain actionable if domain construction
  // evolves.
  for (const auto& [value, physical_register] :
       constraints.fixed_registers) {
    if (const auto allocated = assignment.find(value);
        allocated != assignment.end() &&
        allocated->second != physical_register) {
      diagnostics.push_back(
          value_text(value) + " violates its fixed-register constraint");
    }
  }
}

} // namespace

bool InterferenceGraph::interferes(ir::Id first, ir::Id second) const {
  if (first == second) return false;
  return edges.count(canonical_edge(first, second)) != 0;
}

bool RegisterAllocationEvidence::has_complete_allocation() const {
  return assignment.size() == interference.values.size() &&
         std::all_of(interference.values.begin(), interference.values.end(),
                     [&](const auto value) {
                       return assignment.count(value) != 0;
                     });
}

void validate_calling_convention(const CallingConvention& convention) {
  if (convention.register_count == 0) {
    throw std::invalid_argument(
        "calling convention requires at least one physical register");
  }
  check_register(convention.return_register, convention,
                 "return register");

  std::set<PhysicalRegister> arguments;
  for (const auto physical_register : convention.argument_registers) {
    check_register(physical_register, convention, "argument register");
    if (!arguments.insert(physical_register).second) {
      throw std::invalid_argument(
          "calling convention has duplicate argument registers");
    }
  }

  const auto validate_set =
      [&](const std::set<PhysicalRegister>& registers,
          const char* description) {
        for (const auto physical_register : registers) {
          check_register(physical_register, convention, description);
        }
      };
  validate_set(convention.caller_saved, "caller-saved register");
  validate_set(convention.callee_saved, "callee-saved register");
  validate_set(convention.reserved, "reserved register");

  const auto disjoint = [](const auto& first, const auto& second) {
    return std::none_of(first.begin(), first.end(),
                        [&](const auto value) {
                          return second.count(value) != 0;
                        });
  };
  if (!disjoint(convention.caller_saved, convention.callee_saved) ||
      !disjoint(convention.caller_saved, convention.reserved) ||
      !disjoint(convention.callee_saved, convention.reserved)) {
    throw std::invalid_argument(
        "caller-saved, callee-saved, and reserved registers must be disjoint");
  }

  const auto classified =
      static_cast<std::uint64_t>(convention.caller_saved.size()) +
      static_cast<std::uint64_t>(convention.callee_saved.size()) +
      static_cast<std::uint64_t>(convention.reserved.size());
  if (classified != convention.register_count) {
    throw std::invalid_argument(
        "saved and reserved register sets must partition the physical range");
  }
  if (convention.reserved.count(convention.return_register)) {
    throw std::invalid_argument("return register cannot be reserved");
  }
  for (const auto physical_register : convention.argument_registers) {
    if (convention.reserved.count(physical_register)) {
      throw std::invalid_argument("argument register cannot be reserved");
    }
  }
}

void validate_allocation_constraints(
    const ir::Function& function,
    const CallingConvention& convention,
    const AllocationConstraints& constraints) {
  ir::verify(function);
  validate_calling_convention(convention);
  const auto values = value_ids(function);

  const auto check_value = [&](ir::Id value, const char* description) {
    if (!values.count(value)) {
      throw std::invalid_argument(std::string(description) +
                                  " references an unknown or non-value SSA ID");
    }
  };
  for (const auto& [value, physical_register] :
       constraints.fixed_registers) {
    check_value(value, "fixed-register constraint");
    check_register(physical_register, convention,
                   "fixed-register constraint");
    if (convention.reserved.count(physical_register)) {
      throw std::invalid_argument(
          "fixed-register constraint selects a reserved register");
    }
  }
  for (const auto& [value, registers] :
       constraints.allowed_registers) {
    check_value(value, "allowed-register constraint");
    for (const auto physical_register : registers) {
      check_register(physical_register, convention,
                     "allowed-register constraint");
      if (convention.reserved.count(physical_register)) {
        throw std::invalid_argument(
            "allowed-register constraint names a reserved register");
      }
    }
  }
  for (const auto& [value, registers] :
       constraints.forbidden_registers) {
    check_value(value, "forbidden-register constraint");
    for (const auto physical_register : registers) {
      check_register(physical_register, convention,
                     "forbidden-register constraint");
    }
  }
  for (const auto& [first, second] :
       constraints.additional_interferences) {
    check_value(first, "additional interference");
    check_value(second, "additional interference");
    if (first == second) {
      throw std::invalid_argument(
          "additional interference cannot be a self-edge");
    }
  }
}

InterferenceGraph build_interference_graph(
    const ir::Function& function) {
  ir::verify(function);
  InterferenceGraph graph;
  graph.values = value_ids(function);
  for (const auto value : graph.values) graph.neighbors[value];

  const auto live = ir::liveness(function);
  const auto add_sets = [&](const auto& sets) {
    for (const auto& [location, values] : sets) {
      (void)location;
      for (const auto value : values) {
        if (!graph.values.count(value)) {
          throw std::logic_error(
              "Novostone liveness contains a non-value SSA ID");
        }
      }
      add_clique(graph, values);
    }
  };
  add_sets(live.live_in);
  add_sets(live.live_out);
  add_sets(live.edges);
  add_sets(live.before);
  add_sets(live.after);

  // A Param may appear after ordinary entry-block instructions.  Its incoming
  // ABI register must survive those instructions even though ordinary SSA
  // liveness begins only at the Param pseudo-definition.
  std::set<ir::Id> preceding_entry_values;
  for (const auto& instruction :
       function.blocks.at(function.entry).instructions) {
    if (instruction.op == ir::Op::Param) {
      for (const auto preceding : preceding_entry_values) {
        add_edge(graph, instruction.id, preceding);
      }
    }
    if (ir::produces_value(instruction.op)) {
      preceding_entry_values.insert(instruction.id);
    }
  }

  // A newly defined value and every value live after that definition occupy
  // the same boundary.  This is important for dead definitions, for which the
  // definition itself need not appear in live.after.
  for (const auto& [block_id, block] : function.blocks) {
    (void)block_id;
    for (const auto& instruction : block.instructions) {
      if (!ir::produces_value(instruction.op)) continue;
      const auto after = live.after.find(instruction.id);
      if (after == live.after.end()) {
        throw std::logic_error(
            "Novostone liveness omitted an instruction boundary");
      }
      for (const auto other : after->second) {
        add_edge(graph, instruction.id, other);
      }
    }
  }
  return graph;
}

RegisterAllocationEvidence validate_register_allocation(
    const ir::Function& function,
    const CallingConvention& convention,
    const RegisterAssignment& assignment,
    const AllocationConstraints& constraints,
    AllocationOptions options) {
  validate_options(options);
  auto prepared = prepare(function, convention, constraints);

  RegisterAllocationEvidence evidence;
  evidence.assignment = assignment;
  evidence.interference = prepared.graph;
  evidence.limitations.push_back(native_limitation);
  evidence.diagnostics = prepared.conflicts;
  append_mapping_diagnostics(prepared, convention, constraints, assignment,
                             evidence.diagnostics);

  evidence.structurally_valid = evidence.diagnostics.empty();
  if (evidence.structurally_valid) {
    evidence.preservation_obligations =
        preservation_obligations(convention, assignment);
  }

  if (!evidence.structurally_valid) {
    evidence.status = EvidenceStatus::Rejected;
    evidence.solver_expectation = SolverExpectation::Sat;
  } else if (!evidence.preservation_obligations.empty()) {
    evidence.status = EvidenceStatus::Unknown;
    evidence.solver_expectation = SolverExpectation::Sat;
    evidence.limitations.push_back(
        "callee-saved registers are allocated, but preservation cannot be "
        "established without machine prologue/epilogue evidence");
  } else {
    evidence.status = EvidenceStatus::ObligationReady;
    evidence.solver_expectation = SolverExpectation::Unsat;
  }

  bool within_budget = true;
  evidence.smtlib2 = emit_smt(
      prepared, convention, constraints, assignment,
      evidence.preservation_obligations,
      evidence.structurally_valid,
      evidence.solver_expectation, options, within_budget);
  if (!within_budget) {
    evidence.status = EvidenceStatus::Unknown;
    evidence.solver_expectation = SolverExpectation::Unknown;
    evidence.limitations.push_back(
        "SMT certificate construction exceeded its configured resource limit");
  }
  return evidence;
}

RegisterAllocationEvidence allocate_registers(
    const ir::Function& function,
    const CallingConvention& convention,
    const AllocationConstraints& constraints,
    AllocationOptions options) {
  validate_options(options);
  const auto prepared = prepare(function, convention, constraints);
  if (!prepared.conflicts.empty()) {
    RegisterAllocationEvidence evidence;
    evidence.status = EvidenceStatus::Rejected;
    evidence.solver_expectation = SolverExpectation::Unknown;
    evidence.interference = prepared.graph;
    evidence.diagnostics = prepared.conflicts;
    evidence.limitations.push_back(native_limitation);
    return evidence;
  }

  RegisterAssignment assignment;
  const auto search = color(prepared, convention, options, assignment);
  if (search == SearchResult::Found) {
    return validate_register_allocation(function, convention, assignment,
                                        constraints, options);
  }

  RegisterAllocationEvidence evidence;
  evidence.status = search == SearchResult::Impossible
                        ? EvidenceStatus::Rejected
                        : EvidenceStatus::Unknown;
  evidence.solver_expectation = SolverExpectation::Unknown;
  evidence.interference = prepared.graph;
  evidence.limitations.push_back(native_limitation);
  evidence.diagnostics.push_back(
      search == SearchResult::Impossible
          ? "no register-only allocation satisfies the modeled constraints"
          : "register allocation search exhausted its node limit");
  return evidence;
}

std::string emit_register_allocation_obligation(
    const ir::Function& function,
    const CallingConvention& convention,
    const RegisterAssignment& assignment,
    const AllocationConstraints& constraints,
    AllocationOptions options) {
  return validate_register_allocation(function, convention, assignment,
                                      constraints, options)
      .smtlib2;
}

} // namespace novostone::pcc
