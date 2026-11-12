#pragma once
#include "novostone/CCDB/CCDB.hpp"

#include <optional>

namespace novostone::ir {
using Id = ccdb::Entity;
enum class Op { Param, Const, Copy, Add, Sub, Mul, Eq, Ult, Select, Load, Store, Jump, Branch, Return, Phi };
std::string name(Op op);
Op opcode(const std::string& name);
bool pure(Op op);
bool produces_value(Op op);
struct Effects { bool reads = false; bool writes = false; bool may_trap = false; };
Effects effects(Op op);
struct MemoryDependency { Id block; Id instruction; Id previous; Effects effect; };
struct Instruction {
  Id id = 0;
  Op op = Op::Const;
  std::uint32_t immediate = 0;
  std::vector<Id> operands;
  // For Phi: operands[k] is the value on the incoming edge from targets[k].
  std::vector<Id> targets;
};
struct Block { Id id = 0; std::vector<Instruction> instructions; };
struct Function {
  std::uint32_t arity = 0;
  Id entry = 0;
  std::map<Id, Block> blocks;
};
datalog::Program program();
Function load(const ccdb::Snapshot& snapshot);
void verify(const Function& function);
void verify(const ccdb::Snapshot& snapshot);
std::map<Id, std::set<Id>> dominators(const Function& function);
// Per-block single-state memory chains; previous == 0 is the block entry state.
std::vector<MemoryDependency> memory_dependencies(const Function& function);
struct Liveness {
  std::map<Id, std::set<Id>> live_in, live_out;
  std::map<std::pair<Id, Id>, std::set<Id>> edges;
  // Phis share a parallel before/after boundary, not sequential operand uses.
  std::map<Id, std::set<Id>> before, after;
  ccdb::Revision revision = 0;
  std::string explain() const;
};
Liveness liveness(const ccdb::Snapshot& snapshot, datalog::Limits limits = {});
Liveness liveness(const Function& function, datalog::Limits limits = {});
struct Loop {
  Id header;
  std::set<Id> latches, blocks;
  std::set<std::pair<Id, Id>> exits;
};
// Natural loops only: irreducible cycles need a separate region analysis.
std::vector<Loop> natural_loops(const Function& function);
std::string dump(const Function& function);
Function parse(const std::string& text);
void store(ccdb::Transaction& transaction, const Function& function);
ccdb::Snapshot publish(ccdb::Database& database, const ccdb::Snapshot& base, const Function& function,
                       const std::string& reason);
class Builder {
public:
  explicit Builder(ccdb::Database& database, std::uint32_t arity = 0);
  Id block();
  Id emit(Id block, Op op, std::vector<Id> operands = {}, std::uint32_t immediate = 0,
          std::vector<Id> targets = {});
  void incoming(Id phi, Id predecessor, Id value);
  ccdb::Snapshot finish();
private:
  ccdb::Database& database_;
  ccdb::Snapshot base_;
  Function function_;
};
struct Write { std::uint32_t address; std::uint32_t value; bool operator==(const Write&) const = default; };
struct State {
  Id block = 0;
  std::size_t offset = 0;
  std::map<Id, std::uint32_t> values;
  std::vector<std::uint32_t> memory;
  std::vector<Write> writes;
  std::size_t steps = 0;
  Id predecessor = 0;
  // Captured before assigning any phi: parallel edge semantics, including cycles.
  std::map<Id, std::uint32_t> phi_values;
};
struct Outcome {
  std::optional<std::uint32_t> value;
  std::string trap;
  State state;
  bool suspended = false;
};
// Called before executing each instruction. Returning false suspends at that PC.
using Hook = std::function<bool(const Instruction&, const State&)>;
Outcome resume(const Function& function, const std::vector<std::uint32_t>& arguments,
               State state, std::size_t budget = 100000, Hook hook = {});
Outcome evaluate(const Function& function, const std::vector<std::uint32_t>& arguments = {},
                 std::vector<std::uint32_t> memory = {}, std::size_t budget = 100000);
bool equivalent(const Outcome& a, const Outcome& b);
} // namespace novostone::ir
