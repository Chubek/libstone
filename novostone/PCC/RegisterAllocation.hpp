#pragma once

#include "novostone/DatabaseBasedIR/DatabaseBasedIR.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace novostone::pcc {

using PhysicalRegister = std::uint32_t;
using RegisterAssignment = std::map<ir::Id, PhysicalRegister>;
using InterferenceEdge = std::pair<ir::Id, ir::Id>;

// Registers are numbered [0, register_count).  The saved and reserved sets
// must form a disjoint partition of that range.  Novostone currently models
// register-only arguments and a single register return; stack arguments and
// aggregate returns are deliberately not inferred.
struct CallingConvention {
  std::uint32_t register_count = 0;
  std::vector<PhysicalRegister> argument_registers;
  PhysicalRegister return_register = 0;
  std::set<PhysicalRegister> caller_saved;
  std::set<PhysicalRegister> callee_saved;
  std::set<PhysicalRegister> reserved;

  bool operator==(const CallingConvention&) const = default;
};

// Constraints apply to value-producing SSA instruction IDs.  An allowed set,
// when present, is an allow-list.  Forbidden registers are removed after that
// allow-list is applied.  Additional interference is useful for constraints
// imposed by a later lowering stage, but does not replace liveness-derived
// interference.
struct AllocationConstraints {
  std::map<ir::Id, PhysicalRegister> fixed_registers;
  std::map<ir::Id, std::set<PhysicalRegister>> allowed_registers;
  std::map<ir::Id, std::set<PhysicalRegister>> forbidden_registers;
  std::set<InterferenceEdge> additional_interferences;

  bool operator==(const AllocationConstraints&) const = default;
};

// Search exhaustion is Unknown, never evidence that a coloring is impossible.
// SMT limits bound certificate construction for adversarially large inputs.
struct AllocationOptions {
  std::size_t search_node_limit = 1'000'000;
  std::size_t smt_boolean_variable_limit = 1'000'000;
  std::size_t smt_constraint_limit = 2'000'000;

  bool operator==(const AllocationOptions&) const = default;
};

struct InterferenceGraph {
  std::set<ir::Id> values;
  std::set<InterferenceEdge> edges;
  std::map<ir::Id, std::set<ir::Id>> neighbors;

  [[nodiscard]] bool interferes(ir::Id first, ir::Id second) const;
  bool operator==(const InterferenceGraph&) const = default;
};

// ObligationReady means that the concrete abstract allocation is structurally
// valid and its emitted, negated SMT obligation is expected to be UNSAT.  It
// does not mean a solver has run, nor that native code obeys an ABI.
enum class EvidenceStatus {
  Rejected,
  ObligationReady,
  Unknown
};

enum class SolverExpectation {
  Sat,
  Unsat,
  Unknown
};

struct CalleeSavedPreservation {
  PhysicalRegister physical_register = 0;
  std::set<ir::Id> values;
  std::string predicate;

  bool operator==(const CalleeSavedPreservation&) const = default;
};

struct RegisterAllocationEvidence {
  EvidenceStatus status = EvidenceStatus::Unknown;
  SolverExpectation solver_expectation = SolverExpectation::Unknown;
  RegisterAssignment assignment;
  InterferenceGraph interference;
  std::vector<CalleeSavedPreservation> preservation_obligations;
  std::vector<std::string> diagnostics;
  std::vector<std::string> limitations;
  std::string smtlib2;

  // This covers the SSA coloring, explicit constraints, and abstract
  // parameter/return locations only.
  bool structurally_valid = false;

  // Always false for this abstract API: it is not bound to native code.
  // Native.hpp provides a separate checker for a concrete target subset.
  bool native_calling_convention_proven = false;

  [[nodiscard]] bool has_complete_allocation() const;
};

using Allocation = RegisterAssignment;
using AllocationEvidence = RegisterAllocationEvidence;

// Throws std::invalid_argument for malformed or contradictory conventions.
void validate_calling_convention(const CallingConvention& convention);

// Throws std::invalid_argument for constraints that reference unknown SSA
// values/registers, self-interference, or reserved fixed/allowed registers.
void validate_allocation_constraints(const ir::Function& function,
                                     const CallingConvention& convention,
                                     const AllocationConstraints& constraints = {});

// Builds the canonical undirected graph from Novostone's liveness boundaries,
// live CFG edges, definition/live-after interference, and the virtual
// entry-live interval needed to preserve late Param pseudo-definitions.
InterferenceGraph build_interference_graph(const ir::Function& function);

// Deterministic register-only coloring.  Caller-saved registers are tried
// before callee-saved registers to avoid creating preservation obligations
// when an equally valid caller-saved coloring exists.
RegisterAllocationEvidence allocate_registers(
    const ir::Function& function,
    const CallingConvention& convention,
    const AllocationConstraints& constraints = {},
    AllocationOptions options = {});

// Validates a caller-supplied total allocation.  Mapping errors and constraint
// violations are returned as Rejected evidence rather than being mistaken for
// solver or native-code proofs.
RegisterAllocationEvidence validate_register_allocation(
    const ir::Function& function,
    const CallingConvention& convention,
    const RegisterAssignment& assignment,
    const AllocationConstraints& constraints = {},
    AllocationOptions options = {});

// Convenience entry point for clients that only need the deterministic
// SMT-LIB2 text.  The pure-Boolean script fixes the supplied assignment with
// one-hot selectors, asserts only the negation of all modeled correctness
// conditions (not the conditions themselves), and ends in check-sat.
// Inspect validate_register_allocation() when status/limitations are needed.
std::string emit_register_allocation_obligation(
    const ir::Function& function,
    const CallingConvention& convention,
    const RegisterAssignment& assignment,
    const AllocationConstraints& constraints = {},
    AllocationOptions options = {});

} // namespace novostone::pcc
